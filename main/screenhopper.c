// Screen Hopper (ESP32-S3) — M1: USB host read.
//
// This milestone proves the reason the project moved to the ESP32-S3: the board
// can act as a USB host, enumerate a real mouse and keyboard, and decode their
// input reports. There is no BLE and no absolute-cursor model here yet — those
// arrive in M2. All this firmware does is host HID devices and log what they send.
//
// Devices are driven in the HID *boot* protocol: a mouse gives 8-bit relative
// dx/dy + buttons, a keyboard gives modifiers + up to six keycodes. That is all
// M1 needs. Report-protocol parsing of a device's own HID descriptor (needed for
// wheels, extra buttons, NKRO) is deferred to a later milestone.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_err.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"

#include "usb/usb_host.h"
#include "usb/hid_host.h"
#include "usb/hid_usage_keyboard.h"
#include "usb/hid_usage_mouse.h"

static const char *TAG = "screenhopper";

// The HID host driver reports device connections from its own background task.
// We hand those off to app_main via a queue so that opening/starting a device
// (which issues USB control transfers) happens outside the driver's callback.
typedef struct {
    hid_host_device_handle_t handle;
    hid_host_driver_event_t  event;
} app_event_t;

static QueueHandle_t app_event_queue = NULL;

static const char *proto_name(uint8_t proto)
{
    switch (proto) {
    case HID_PROTOCOL_KEYBOARD: return "keyboard";
    case HID_PROTOCOL_MOUSE:    return "mouse";
    default:                    return "generic HID";
    }
}

// ---------------------------------------------------------------- decoding

static void log_mouse_report(const uint8_t *data, size_t len)
{
    if (len < sizeof(hid_mouse_input_report_boot_t)) {
        return;
    }
    const hid_mouse_input_report_boot_t *r =
        (const hid_mouse_input_report_boot_t *)data;

    ESP_LOGI(TAG, "mouse   [%c%c%c] dx=%4d dy=%4d",
             r->buttons.button1 ? 'L' : '.',
             r->buttons.button2 ? 'R' : '.',
             r->buttons.button3 ? 'M' : '.',
             r->x_displacement, r->y_displacement);
}

// Translate a HID keyboard usage code to a printable character, or 0 if we
// don't have a mapping (those are shown as [XX] hex instead).
static char keycode_to_ascii(uint8_t code, bool shift)
{
    if (code >= 0x04 && code <= 0x1D) {           // a..z
        char c = 'a' + (code - 0x04);
        return shift ? (char)(c - 'a' + 'A') : c;
    }
    if (code >= 0x1E && code <= 0x26) return '1' + (code - 0x1E);  // 1..9
    if (code == 0x27) return '0';
    if (code == 0x2C) return ' ';
    return 0;
}

static void log_keyboard_report(const uint8_t *data, size_t len)
{
    if (len < sizeof(hid_keyboard_input_report_boot_t)) {
        return;
    }
    const hid_keyboard_input_report_boot_t *r =
        (const hid_keyboard_input_report_boot_t *)data;

    const uint8_t mod = r->modifier.val;
    const bool shift = mod & 0x22;   // bit1 = left shift, bit5 = right shift

    char keys[64];
    size_t n = 0;
    for (int i = 0; i < 6 && n + 4 < sizeof(keys); i++) {
        const uint8_t code = r->key[i];
        if (code == 0) {
            continue;
        }
        const char c = keycode_to_ascii(code, shift);
        if (c) {
            keys[n++] = c;
        } else {
            n += snprintf(keys + n, sizeof(keys) - n, "[%02X]", code);
        }
    }
    keys[n] = '\0';

    ESP_LOGI(TAG, "keyboard mod=0x%02X keys=\"%s\"", mod, keys);
}

// ---------------------------------------------------------------- callbacks

// Per-interface events for an opened device: input reports and disconnects.
static void hid_host_interface_callback(hid_host_device_handle_t handle,
                                        const hid_host_interface_event_t event,
                                        void *arg)
{
    (void)arg;
    hid_host_dev_params_t params;
    ESP_ERROR_CHECK(hid_host_device_get_params(handle, &params));

    switch (event) {
    case HID_HOST_INTERFACE_EVENT_INPUT_REPORT: {
        uint8_t data[64];
        size_t len = 0;
        ESP_ERROR_CHECK(hid_host_device_get_raw_input_report_data(
            handle, data, sizeof(data), &len));

        if (params.proto == HID_PROTOCOL_MOUSE) {
            log_mouse_report(data, len);
        } else if (params.proto == HID_PROTOCOL_KEYBOARD) {
            log_keyboard_report(data, len);
        }
        break;
    }
    case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "%s disconnected", proto_name(params.proto));
        ESP_ERROR_CHECK(hid_host_device_close(handle));
        break;
    case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
        ESP_LOGW(TAG, "%s transfer error", proto_name(params.proto));
        break;
    default:
        ESP_LOGW(TAG, "unhandled interface event %d", event);
        break;
    }
}

// A new device was connected: open it, put it in boot protocol, and start it.
static void handle_device_connected(hid_host_device_handle_t handle)
{
    hid_host_dev_params_t params;
    ESP_ERROR_CHECK(hid_host_device_get_params(handle, &params));
    ESP_LOGI(TAG, "%s connected", proto_name(params.proto));

    const hid_host_device_config_t dev_config = {
        .callback = hid_host_interface_callback,
        .callback_arg = NULL,
    };
    ESP_ERROR_CHECK(hid_host_device_open(handle, &dev_config));

    if (params.sub_class == HID_SUBCLASS_BOOT_INTERFACE) {
        ESP_ERROR_CHECK(hid_class_request_set_protocol(handle, HID_REPORT_PROTOCOL_BOOT));
        if (params.proto == HID_PROTOCOL_KEYBOARD) {
            ESP_ERROR_CHECK(hid_class_request_set_idle(handle, 0, 0));
        }
    } else {
        ESP_LOGW(TAG, "device has no boot interface; M1 only decodes boot reports");
    }

    ESP_ERROR_CHECK(hid_host_device_start(handle));
}

// Runs in the HID host driver's background task — keep it light: just forward
// the connection event to app_main.
static void hid_host_device_callback(hid_host_device_handle_t handle,
                                     const hid_host_driver_event_t event,
                                     void *arg)
{
    (void)arg;
    const app_event_t evt = { .handle = handle, .event = event };
    xQueueSend(app_event_queue, &evt, 0);
}

// ---------------------------------------------------------------- USB daemon

// Owns the USB host library: installs it, then pumps its event loop forever.
static void usb_lib_task(void *arg)
{
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_config));
    xTaskNotifyGive((TaskHandle_t)arg);   // tell app_main the host is up

    while (true) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            ESP_ERROR_CHECK(usb_host_device_free_all());
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Screen Hopper (ESP32-S3) — M1 USB host read");

    app_event_queue = xQueueCreate(10, sizeof(app_event_t));
    assert(app_event_queue != NULL);

    BaseType_t created = xTaskCreatePinnedToCore(
        usb_lib_task, "usb_lib", 4096, xTaskGetCurrentTaskHandle(), 2, NULL, 0);
    assert(created == pdTRUE);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);   // wait for usb_host_install()

    const hid_host_driver_config_t hid_host_config = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .core_id = 0,
        .callback = hid_host_device_callback,
        .callback_arg = NULL,
    };
    ESP_ERROR_CHECK(hid_host_install(&hid_host_config));

    ESP_LOGI(TAG, "ready — plug in a USB mouse or keyboard");

    app_event_t evt;
    while (true) {
        if (xQueueReceive(app_event_queue, &evt, portMAX_DELAY)) {
            if (evt.event == HID_HOST_DRIVER_EVENT_CONNECTED) {
                handle_device_connected(evt.handle);
            }
        }
    }
}
