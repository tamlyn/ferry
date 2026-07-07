#include "ble_hid.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"

#include "esp_bt.h"
#include "esp_bt_defs.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"

#include "esp_hidd.h"
#include "esp_hidd_gatts.h"
#include "esp_hid_common.h"

#include "our_descriptor.h"

static const char *TAG = "ble_hid";

static esp_hidd_dev_t *s_hid_dev = NULL;

// Set once the link is encrypted, cleared on disconnect. Input reports sent
// before encryption are ignored by the host, so gate every send on this.
static volatile bool s_encrypted = false;

static esp_hid_raw_report_map_t s_report_maps[] = {
    { .data = our_report_descriptor, .len = 0 /* filled in at init */ },
};

static esp_hid_device_config_t s_hid_config = {
    .vendor_id         = 0x16C0,
    .product_id        = 0x05DF,
    .version           = 0x0100,
    .device_name       = "Screen Hopper",
    .manufacturer_name = "Screen Hopper",
    .serial_number     = "1",
    .report_maps       = s_report_maps,
    .report_maps_len   = 1,
};

// The HID service UUID (0x1812), advertised so hosts recognise us as a HID
// peripheral. Bluedroid requires the full 128-bit expansion here — it iterates
// the list in 16-byte chunks and re-emits base UUIDs in their short 16-bit form.
static uint8_t s_hid_service_uuid128[] = {
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x12, 0x18, 0x00, 0x00,
};

// The advertising packet carries the HID identity (flags, service UUID,
// appearance, tx power); the device name goes in the scan response so it is
// never truncated by the 31-byte advert limit.
static esp_ble_adv_data_t s_adv_data = {
    .set_scan_rsp     = false,
    .include_name     = false,
    .include_txpower  = true,
    .min_interval     = 0x0006,   // preferred connection interval, x1.25ms
    .max_interval     = 0x0010,
    .appearance       = ESP_HID_APPEARANCE_GENERIC,   // combined mouse+keyboard
    .service_uuid_len = sizeof(s_hid_service_uuid128),
    .p_service_uuid   = s_hid_service_uuid128,
    .flag             = 0x6,       // LE General Discoverable, BR/EDR not supported
};

static esp_ble_adv_data_t s_scan_rsp_data = {
    .set_scan_rsp = true,
    .include_name = true,
};

static esp_ble_adv_params_t s_adv_params = {
    .adv_int_min       = 0x20,
    .adv_int_max       = 0x30,
    .adv_type          = ADV_TYPE_IND,
    .own_addr_type     = BLE_ADDR_TYPE_PUBLIC,
    .channel_map       = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static void ble_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_SEC_REQ_EVT:
        // A host asked to secure the link; with Just Works there is nothing to
        // confirm, so accept.
        esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
        break;

    case ESP_GAP_BLE_AUTH_CMPL_EVT:
        if (param->ble_security.auth_cmpl.success) {
            s_encrypted = true;
            ESP_LOGI(TAG, "bonded and encrypted — ready to send input");
        } else {
            ESP_LOGE(TAG, "pairing failed, reason 0x%x",
                     param->ble_security.auth_cmpl.fail_reason);
        }
        break;

    default:
        break;
    }
}

// esp_hidd device lifecycle. START fires once the GATT database is up; from then
// on we advertise whenever no host is connected.
static void hidd_event_cb(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)arg;
    (void)base;
    (void)event_data;
    esp_hidd_event_t event = (esp_hidd_event_t)id;

    switch (event) {
    case ESP_HIDD_START_EVENT:
        ESP_LOGI(TAG, "HID stack started, advertising");
        esp_ble_gap_start_advertising(&s_adv_params);
        break;

    case ESP_HIDD_CONNECT_EVENT:
        ESP_LOGI(TAG, "host connected");
        break;

    case ESP_HIDD_DISCONNECT_EVENT:
        ESP_LOGI(TAG, "host disconnected, re-advertising");
        s_encrypted = false;
        esp_ble_gap_start_advertising(&s_adv_params);
        break;

    default:
        break;
    }
}

esp_err_t ble_hid_init(void)
{
    s_report_maps[0].len = our_report_descriptor_length;

    // Controller: BLE only. Reclaim the Classic-BT controller memory we will
    // never use.
    ESP_RETURN_ON_ERROR(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT),
                        TAG, "mem_release");
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_bt_controller_init(&bt_cfg), TAG, "controller_init");
    ESP_RETURN_ON_ERROR(esp_bt_controller_enable(ESP_BT_MODE_BLE), TAG, "controller_enable");

    // Host stack (Bluedroid).
    esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_bluedroid_init_with_cfg(&bluedroid_cfg), TAG, "bluedroid_init");
    ESP_RETURN_ON_ERROR(esp_bluedroid_enable(), TAG, "bluedroid_enable");

    ESP_RETURN_ON_ERROR(esp_ble_gap_register_callback(ble_gap_cb), TAG, "gap_register");
    ESP_RETURN_ON_ERROR(esp_ble_gatts_register_callback(esp_hidd_gatts_event_handler),
                        TAG, "gatts_register");

    // Security: bonded + encrypted with Secure Connections, but Just Works — the
    // device has no keypad or display, so no MITM/passkey. Keys persist in NVS so
    // paired hosts reconnect without re-pairing.
    esp_ble_auth_req_t auth_req = ESP_LE_AUTH_REQ_SC_BOND;
    esp_ble_io_cap_t   iocap    = ESP_IO_CAP_NONE;
    uint8_t key_size = 16;
    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t rsp_key  = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(auth_req));
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(iocap));
    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(key_size));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(init_key));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(rsp_key));

    ESP_RETURN_ON_ERROR(esp_ble_gap_set_device_name(s_hid_config.device_name),
                        TAG, "set_device_name");
    ESP_RETURN_ON_ERROR(esp_ble_gap_config_adv_data(&s_adv_data), TAG, "config_adv_data");
    ESP_RETURN_ON_ERROR(esp_ble_gap_config_adv_data(&s_scan_rsp_data), TAG, "config_scan_rsp");

    // Registers the HID/Battery/Device-Info GATT services from our report map and
    // starts the profile; the START event above kicks off advertising.
    ESP_RETURN_ON_ERROR(
        esp_hidd_dev_init(&s_hid_config, ESP_HID_TRANSPORT_BLE, hidd_event_cb, &s_hid_dev),
        TAG, "hidd_dev_init");

    return ESP_OK;
}

bool ble_hid_ready(void)
{
    return s_encrypted && s_hid_dev != NULL && esp_hidd_dev_connected(s_hid_dev);
}

esp_err_t ble_hid_send_mouse(uint8_t buttons, uint16_t x, uint16_t y)
{
    if (!ble_hid_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t report[MOUSE_REPORT_SIZE] = {
        buttons,
        (uint8_t)(x & 0xFF), (uint8_t)(x >> 8),
        (uint8_t)(y & 0xFF), (uint8_t)(y >> 8),
    };
    return esp_hidd_dev_input_set(s_hid_dev, 0, REPORT_ID_MOUSE, report, sizeof(report));
}

esp_err_t ble_hid_send_keyboard(uint8_t modifiers, const uint8_t keys[6])
{
    if (!ble_hid_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t report[KEYBOARD_REPORT_SIZE] = {0};
    report[0] = modifiers;   // report[1] stays 0 (reserved byte)
    memcpy(&report[2], keys, 6);
    return esp_hidd_dev_input_set(s_hid_dev, 0, REPORT_ID_KEYBOARD, report, sizeof(report));
}
