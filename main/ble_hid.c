// BLE HID-over-GATT peripheral on the NimBLE host stack, holding up to two live
// host connections at once (a two-machine KVM).
//
// Why NimBLE rather than Bluedroid: HOGP CCCD (notification-enabled) state must
// be tracked *per connection* for two hosts to each subscribe independently.
// NimBLE does this by construction; Bluedroid keeps one shared CCCD value, so a
// second host reads back the first's "enabled" state and never subscribes. We
// also lean on NimBLE's built-in HOGP service builder (ble_svc_hid) instead of
// hand-building the attribute table.
//
// esp_hid's device layer is single-host on both stacks ("there can be only one
// BLE HID device"), so the connection/send layer here is our own regardless.

#include "ble_hid.h"

#include <assert.h>
#include <string.h>

#include "esp_log.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "services/bas/ble_svc_bas.h"
#include "services/dis/ble_svc_dis.h"
#include "services/hid/ble_svc_hid.h"

#include "our_descriptor.h"

static const char *TAG = "ble_hid";

#define DEVICE_NAME "Screen Hopper"

// HID appearance (0x03C0 = HID Generic), advertised so hosts recognise us as a
// combined mouse+keyboard HID peripheral.
#define HID_APPEARANCE 0x03C0

// USB PnP identity presented in the Device Information Service — the shared
// V-USB VID/PID, matching what prior hosts accepted.
#define PNP_VENDOR_ID  0x16C0
#define PNP_PRODUCT_ID 0x05DF
#define PNP_VERSION    0x0100

// HID Information characteristic value: bcdHID 1.11, no country code, and the
// RemoteWake | NormallyConnectable flags.
static const uint8_t HID_INFO[4] = { 0x11, 0x01, 0x00, 0x03 };

// The device's own advertising address type, resolved once at host sync.
static uint8_t s_own_addr_type;

// GATT value handles for our two input-report characteristics, captured from the
// registration callback below. Notifications are sent against these handles.
static uint16_t s_mouse_val_handle;
static uint16_t s_kbd_val_handle;
static uint16_t s_mouse_rel_val_handle;   // relative-pointer report (id 3)
// Report characteristics all share UUID 0x2A4D and register in the order we list
// them in params.rpts[]; this counts them so we can tell mouse from keyboard.
static int s_report_chr_seen;

// One slot per host we can hold live at once. Written from the NimBLE host task
// (GAP events); read from the USB input task (sends). The fields are plain
// word-sized values and a stale read only costs one dropped/extra report during
// a connect/disconnect transition, so no lock is needed.
typedef struct {
    bool     in_use;
    uint16_t conn_handle;
    bool     mouse_sub;   // host enabled notifications on the mouse report
    bool     kbd_sub;     // host enabled notifications on the keyboard report
    bool     mouse_rel_sub;   // host enabled notifications on the relative-pointer report
} host_slot_t;

static host_slot_t s_hosts[BLE_HID_MAX_HOSTS];

// ble_store_config_init() has no public header (per the NimBLE examples); it wires
// up the NVS-backed bond store.
void ble_store_config_init(void);

// ---- slot helpers ---------------------------------------------------------

static host_slot_t *slot_by_conn(uint16_t conn_handle)
{
    for (int i = 0; i < BLE_HID_MAX_HOSTS; i++) {
        if (s_hosts[i].in_use && s_hosts[i].conn_handle == conn_handle) {
            return &s_hosts[i];
        }
    }
    return NULL;
}

// Find the connection's slot, allocating one on first sight. Events for a new
// connection do not begin with BLE_GAP_EVENT_CONNECT on this NimBLE fork: the
// host defers it behind a remote version/feature exchange, so a bonded peer's
// ENC_CHANGE — and the SUBSCRIBE events restoring its persisted CCCDs — arrive
// first. Keying allocation off any event that names the connection means those
// restored subscriptions land in the slot instead of being dropped.
static host_slot_t *slot_find_or_alloc(uint16_t conn_handle)
{
    host_slot_t *s = slot_by_conn(conn_handle);
    if (s) {
        return s;
    }
    for (int i = 0; i < BLE_HID_MAX_HOSTS; i++) {
        if (!s_hosts[i].in_use) {
            s_hosts[i] = (host_slot_t){ .in_use = true, .conn_handle = conn_handle };
            return &s_hosts[i];
        }
    }
    return NULL;
}

static int free_slots(void)
{
    int n = 0;
    for (int i = 0; i < BLE_HID_MAX_HOSTS; i++) {
        if (!s_hosts[i].in_use) {
            n++;
        }
    }
    return n;
}

// ---- advertising ----------------------------------------------------------

static int ble_gap_event(struct ble_gap_event *event, void *arg);

// Advertise connectable + undirected while at least one slot is free. The HID
// identity (flags, appearance, HID service UUID, tx power) goes in the advert;
// the device name goes in the scan response so it is never truncated by the
// 31-byte advert limit.
static void advertise_if_slot_free(void)
{
    if (free_slots() == 0 || ble_gap_adv_active()) {
        return;
    }

    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.appearance = HID_APPEARANCE;
    fields.appearance_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.tx_pwr_lvl_is_present = 1;
    fields.uuids16 = (ble_uuid16_t[]){ BLE_UUID16_INIT(BLE_SVC_HID_UUID16) };
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields failed; rc=%d", rc);
        return;
    }

    struct ble_hs_adv_fields rsp = {0};
    const char *name = ble_svc_gap_device_name();
    rsp.name = (uint8_t *)name;
    rsp.name_len = strlen(name);
    rsp.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_rsp_set_fields failed; rc=%d", rc);
        return;
    }

    struct ble_gap_adv_params adv_params = {0};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, ble_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_start failed; rc=%d", rc);
        return;
    }
    ESP_LOGI(TAG, "advertising as \"%s\" (%d slot(s) free)", name, free_slots());
}

// ---- GAP events -----------------------------------------------------------

// Log the link's negotiated connection parameters — the interval bounds how
// often we can deliver reports, so it's the first thing to check when cursor
// motion stutters.
static void log_conn_params(const char *when, uint16_t conn_handle)
{
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(conn_handle, &desc) != 0) {
        return;
    }
    unsigned itvl_us = desc.conn_itvl * 1250;
    ESP_LOGI(TAG, "%s (conn=%d): interval=%u.%02ums latency=%u timeout=%ums",
             when, conn_handle, itvl_us / 1000, (itvl_us % 1000) / 10,
             desc.conn_latency, desc.supervision_timeout * 10);
}

static int ble_gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            host_slot_t *s = slot_find_or_alloc(event->connect.conn_handle);
            ESP_LOGI(TAG, "host connected (conn=%d) -> slot %d",
                     event->connect.conn_handle, s ? (int)(s - s_hosts) : -1);
            log_conn_params("conn params", event->connect.conn_handle);
        } else {
            ESP_LOGW(TAG, "connect failed; status=%d", event->connect.status);
            // A connection that dies during establishment gets a failed CONNECT
            // instead of a DISCONNECT, but its earlier enc/subscribe events may
            // already have claimed a slot — release it or it leaks.
            host_slot_t *s = slot_by_conn(event->connect.conn_handle);
            if (s) {
                *s = (host_slot_t){0};
            }
        }
        // NimBLE stops advertising on connect; restart if a slot remains free so
        // a second host can find us.
        advertise_if_slot_free();
        return 0;

    case BLE_GAP_EVENT_DISCONNECT: {
        host_slot_t *s = slot_by_conn(event->disconnect.conn.conn_handle);
        if (s) {
            ESP_LOGI(TAG, "host disconnected (conn=%d, reason=%d), freeing slot",
                     event->disconnect.conn.conn_handle, event->disconnect.reason);
            *s = (host_slot_t){0};
        }
        advertise_if_slot_free();
        return 0;
    }

    case BLE_GAP_EVENT_SUBSCRIBE: {
        // TERM fires while the connection is being torn down — never allocate
        // for it; the slot (if any) is about to be freed by DISCONNECT.
        host_slot_t *s = (event->subscribe.reason == BLE_GAP_SUBSCRIBE_REASON_TERM)
                             ? slot_by_conn(event->subscribe.conn_handle)
                             : slot_find_or_alloc(event->subscribe.conn_handle);
        if (s) {
            if (event->subscribe.attr_handle == s_mouse_val_handle) {
                s->mouse_sub = event->subscribe.cur_notify;
            } else if (event->subscribe.attr_handle == s_kbd_val_handle) {
                s->kbd_sub = event->subscribe.cur_notify;
            } else if (event->subscribe.attr_handle == s_mouse_rel_val_handle) {
                s->mouse_rel_sub = event->subscribe.cur_notify;
            }
            ESP_LOGI(TAG, "subscribe (conn=%d attr=%d notify=%d): mouse=%d kbd=%d rel=%d",
                     event->subscribe.conn_handle, event->subscribe.attr_handle,
                     event->subscribe.cur_notify, s->mouse_sub, s->kbd_sub,
                     s->mouse_rel_sub);
        }
        return 0;
    }

    case BLE_GAP_EVENT_ENC_CHANGE: {
        if (event->enc_change.status == 0) {
            slot_find_or_alloc(event->enc_change.conn_handle);
        }
        struct ble_gap_conn_desc desc;
        int rc = ble_gap_conn_find(event->enc_change.conn_handle, &desc);
        ESP_LOGI(TAG, "encryption change (conn=%d): status=%d encrypted=%d bonded=%d",
                 event->enc_change.conn_handle, event->enc_change.status,
                 rc == 0 ? desc.sec_state.encrypted : -1,
                 rc == 0 ? desc.sec_state.bonded : -1);
        return 0;
    }

    case BLE_GAP_EVENT_CONN_UPDATE:
        log_conn_params("conn params updated", event->conn_update.conn_handle);
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        // The host lost its bond but is re-pairing. Drop the stale bond and let
        // the new pairing proceed (hosts cache GATT/CCCD per bond, so a clean
        // re-pair also clears any masked-fix state).
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    default:
        return 0;
    }
}

// ---- GATT services --------------------------------------------------------

// Capture our two input-report value handles as the GATT database registers.
static void gatt_svr_register_cb(struct ble_gatt_register_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_REGISTER_OP_CHR) {
        return;
    }
    if (ble_uuid_u16(ctxt->chr.chr_def->uuid) != BLE_SVC_HID_CHR_UUID16_RPT) {
        return;
    }
    // Report characteristics register in the order we populate params.rpts[]:
    // 0 = mouse input (id 1), 1 = keyboard input (id 2), 2 = keyboard LED output,
    // 3 = relative-pointer input (id 3).
    switch (s_report_chr_seen++) {
    case 0:
        s_mouse_val_handle = ctxt->chr.val_handle;
        break;
    case 1:
        s_kbd_val_handle = ctxt->chr.val_handle;
        break;
    case 3:
        s_mouse_rel_val_handle = ctxt->chr.val_handle;
        break;
    default:
        break;   // case 2: LED output report — the host writes it; we never notify it.
    }
}

static void hid_service_add(void)
{
    // ~1.3 KB — keep it off the (modest) main-task stack.
    static struct ble_svc_hid_params params;
    memset(&params, 0, sizeof(params));

    memcpy(&params.hid_info, HID_INFO, sizeof(HID_INFO));   // hid_info is a uint32_t

    memcpy(params.report_map, our_report_descriptor, our_report_descriptor_length);
    params.report_map_len = our_report_descriptor_length;
    // External Report Reference points at the Battery Service (HOGP convention).
    params.external_rpt_ref = BLE_SVC_BAS_UUID16;

    params.proto_mode_present = 1;
    params.proto_mode = BLE_SVC_HID_PROTO_MODE_REPORT;

    // Report-mode characteristics, in the order gatt_svr_register_cb expects.
    params.rpts[0] = (struct report){
        .type = BLE_SVC_HID_RPT_TYPE_INPUT,  .id = REPORT_ID_MOUSE,    .len = MOUSE_REPORT_SIZE };
    params.rpts[1] = (struct report){
        .type = BLE_SVC_HID_RPT_TYPE_INPUT,  .id = REPORT_ID_KEYBOARD, .len = KEYBOARD_REPORT_SIZE };
    params.rpts[2] = (struct report){
        .type = BLE_SVC_HID_RPT_TYPE_OUTPUT, .id = REPORT_ID_KEYBOARD, .len = 1 };
    params.rpts[3] = (struct report){   // relative pointer, registers 4th (case 3)
        .type = BLE_SVC_HID_RPT_TYPE_INPUT,  .id = REPORT_ID_MOUSE_REL, .len = MOUSE_REL_REPORT_SIZE };
    params.rpts_len = 4;

    int rc = ble_svc_hid_add(params);
    assert(rc == 0);
}

static void gatt_svr_init(void)
{
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_bas_init();

    ble_svc_dis_init();
    static const uint8_t pnp[7] = {
        0x02,   // vendor id source: USB
        PNP_VENDOR_ID  & 0xFF, (PNP_VENDOR_ID  >> 8) & 0xFF,
        PNP_PRODUCT_ID & 0xFF, (PNP_PRODUCT_ID >> 8) & 0xFF,
        PNP_VERSION    & 0xFF, (PNP_VERSION    >> 8) & 0xFF,
    };
    ble_svc_dis_pnp_id_set((const char *)pnp);
    ble_svc_dis_manufacturer_name_set(DEVICE_NAME);
    ble_svc_dis_serial_number_set("1");

    hid_service_add();
    ble_svc_hid_init();
}

// ---- host lifecycle -------------------------------------------------------

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);   // prefer a public identity address
    assert(rc == 0);
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "no usable address; rc=%d", rc);
        return;
    }

    if (s_mouse_val_handle == 0 || s_kbd_val_handle == 0) {
        ESP_LOGE(TAG, "report handles not captured (mouse=%d kbd=%d)",
                 s_mouse_val_handle, s_kbd_val_handle);
    } else {
        ESP_LOGI(TAG, "report handles: mouse=%d kbd=%d rel=%d",
                 s_mouse_val_handle, s_kbd_val_handle, s_mouse_rel_val_handle);
    }

    advertise_if_slot_free();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "nimble host reset; reason=%d", reason);
}

static void host_task(void *param)
{
    ESP_LOGI(TAG, "nimble host task started");
    nimble_port_run();          // returns only on nimble_port_stop()
    nimble_port_freertos_deinit();
}

esp_err_t ble_hid_init(void)
{
    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %d", ret);
        return ret;
    }

    // Security: Just Works (the device has no display/keypad), bonded, Secure
    // Connections. Keys persist in NVS (CONFIG_BT_NIMBLE_NVS_PERSIST) so paired
    // hosts reconnect without re-pairing.
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_our_key_dist   = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.gatts_register_cb = gatt_svr_register_cb;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    gatt_svr_init();

    int rc = ble_svc_gap_device_name_set(DEVICE_NAME);
    assert(rc == 0);
    rc = ble_svc_gap_device_appearance_set(HID_APPEARANCE);
    assert(rc == 0);

    ble_store_config_init();     // NVS-backed bond storage

    nimble_port_freertos_init(host_task);

    return ESP_OK;
}

// ---- per-host transport ---------------------------------------------------

bool ble_hid_ready(int host)
{
    if (host < 0 || host >= BLE_HID_MAX_HOSTS) {
        return false;
    }
    const host_slot_t *s = &s_hosts[host];
    return s->in_use && s->mouse_sub && s->kbd_sub;
}

esp_err_t ble_hid_send_mouse(int host, uint8_t buttons, uint16_t x, uint16_t y, int8_t wheel)
{
    if (host < 0 || host >= BLE_HID_MAX_HOSTS) {
        return ESP_ERR_INVALID_ARG;
    }
    host_slot_t *s = &s_hosts[host];
    if (!s->in_use || !s->mouse_sub) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t report[MOUSE_REPORT_SIZE] = {
        buttons,
        (uint8_t)(x & 0xFF), (uint8_t)(x >> 8),
        (uint8_t)(y & 0xFF), (uint8_t)(y >> 8),
        (uint8_t)wheel,
    };
    struct os_mbuf *om = ble_hs_mbuf_from_flat(report, sizeof(report));
    if (om == NULL) {
        return ESP_ERR_NO_MEM;
    }
    // NimBLE frees om, and only puts it on the wire for this conn if it subscribed.
    int rc = ble_gatts_notify_custom(s->conn_handle, s_mouse_val_handle, om);
    if (rc != 0) {
        // Dropped reports are invisible to the user beyond a stutter (the
        // position is absolute, so the next report corrects it) — count them
        // and log sparsely so a starving link shows up in the console.
        static unsigned drops;
        if ((++drops & 0x7F) == 1) {
            ESP_LOGW(TAG, "mouse notify failed (conn=%d rc=%d, %u drops total)",
                     s->conn_handle, rc, drops);
        }
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t ble_hid_send_mouse_rel(int host, uint8_t buttons, int8_t dx, int8_t dy)
{
    if (host < 0 || host >= BLE_HID_MAX_HOSTS) {
        return ESP_ERR_INVALID_ARG;
    }
    host_slot_t *s = &s_hosts[host];
    if (!s->in_use || !s->mouse_rel_sub) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t report[MOUSE_REL_REPORT_SIZE] = { buttons, (uint8_t)dx, (uint8_t)dy };
    struct os_mbuf *om = ble_hs_mbuf_from_flat(report, sizeof(report));
    if (om == NULL) {
        return ESP_ERR_NO_MEM;
    }
    int rc = ble_gatts_notify_custom(s->conn_handle, s_mouse_rel_val_handle, om);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t ble_hid_send_keyboard(int host, uint8_t modifiers, const uint8_t keys[6])
{
    if (host < 0 || host >= BLE_HID_MAX_HOSTS) {
        return ESP_ERR_INVALID_ARG;
    }
    host_slot_t *s = &s_hosts[host];
    if (!s->in_use || !s->kbd_sub) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t report[KEYBOARD_REPORT_SIZE] = {0};
    report[0] = modifiers;   // report[1] stays 0 (reserved byte)
    memcpy(&report[2], keys, 6);
    struct os_mbuf *om = ble_hs_mbuf_from_flat(report, sizeof(report));
    if (om == NULL) {
        return ESP_ERR_NO_MEM;
    }
    int rc = ble_gatts_notify_custom(s->conn_handle, s_kbd_val_handle, om);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}
