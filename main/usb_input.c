// USB host input: enumerate a boot-protocol mouse and/or keyboard (directly or
// through a hub) and deliver decoded HID reports to the app via callbacks.
//
// Devices are driven in the HID *boot* protocol: a mouse gives 8-bit relative
// dx/dy + buttons, a keyboard gives modifiers + up to six keycodes. The spec's
// boot mouse report is 3 bytes with no wheel, but nearly every real mouse
// appends a signed wheel byte anyway, which we read opportunistically (see
// dispatch_mouse_report). Full report-protocol parsing of a device's own
// descriptor (extra buttons, horizontal pan, NKRO) is a later milestone.

#include "usb_input.h"

#include <stdbool.h>
#include <stddef.h>

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

static const char *TAG = "usb_input";

static usb_mouse_report_cb    s_on_mouse    = NULL;
static usb_keyboard_report_cb s_on_keyboard = NULL;

// Counters for the freeze hunt: how many raw HID reports the USB host driver has
// handed us, split by device, plus transfer errors. diag.c logs their deltas, so a
// freeze where these stay flat while the user is actively typing/moving = the USB
// input path has wedged (the reports simply stop arriving); deltas still rising
// means input reaches us and the stall is downstream (BLE).
static volatile uint32_t s_mouse_reports;
static volatile uint32_t s_kbd_reports;
static volatile uint32_t s_xfer_errors;

// The HID host driver reports device connections from its own background task.
// We hand those off to our event task via a queue so that opening/starting a
// device (which issues USB control transfers) happens outside the driver's
// callback.
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

static void dispatch_mouse_report(const uint8_t *data, size_t len)
{
    if (len < sizeof(hid_mouse_input_report_boot_t) || s_on_mouse == NULL) {
        return;
    }
    const hid_mouse_input_report_boot_t *r =
        (const hid_mouse_input_report_boot_t *)data;

    // Pack the boot report's button bits into a bitmask matching our BLE report
    // (bit0 = left, bit1 = right, bit2 = middle).
    uint8_t buttons = (r->buttons.button1 ? 0x01 : 0) |
                      (r->buttons.button2 ? 0x02 : 0) |
                      (r->buttons.button3 ? 0x04 : 0);

    // The 3-byte boot report carries no wheel, but nearly every mouse appends a
    // signed wheel byte as a 4th byte. Read it when present; mice that omit it
    // simply never scroll.
    int wheel = (len > sizeof(hid_mouse_input_report_boot_t))
                    ? (int8_t)data[sizeof(hid_mouse_input_report_boot_t)]
                    : 0;
    s_on_mouse(buttons, r->x_displacement, r->y_displacement, wheel);
}

static void dispatch_keyboard_report(const uint8_t *data, size_t len)
{
    if (len < sizeof(hid_keyboard_input_report_boot_t) || s_on_keyboard == NULL) {
        return;
    }
    const hid_keyboard_input_report_boot_t *r =
        (const hid_keyboard_input_report_boot_t *)data;
    s_on_keyboard(r->modifier.val, r->key);
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
            s_mouse_reports++;
            dispatch_mouse_report(data, len);
        } else if (params.proto == HID_PROTOCOL_KEYBOARD) {
            s_kbd_reports++;
            dispatch_keyboard_report(data, len);
        }
        break;
    }
    case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "%s disconnected", proto_name(params.proto));
        ESP_ERROR_CHECK(hid_host_device_close(handle));
        break;
    case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
        s_xfer_errors++;
        ESP_LOGW(TAG, "%s transfer error", proto_name(params.proto));
        break;
    default:
        ESP_LOGW(TAG, "unhandled interface event %d", event);
        break;
    }
}

// A new device was connected. We only host boot-protocol mice and keyboards, so
// ignore every other HID interface — a keyboard's extra media-key interface, a
// mouse's vendor interface, a joystick. Besides being undecodable here, claiming
// them ties up the ESP32-S3's limited USB host channels on pipes we never read,
// which is exactly what starves a second device sharing a hub.
//
// A device we cannot drive — an unsupported low-speed device, a quirky one that
// stalls a control transfer — must not take down the host: we log and skip just
// that device rather than aborting the whole board.
static void handle_device_connected(hid_host_device_handle_t handle)
{
    hid_host_dev_params_t params;
    ESP_ERROR_CHECK(hid_host_device_get_params(handle, &params));

    if (params.proto != HID_PROTOCOL_KEYBOARD && params.proto != HID_PROTOCOL_MOUSE) {
        ESP_LOGI(TAG, "ignoring %s interface", proto_name(params.proto));
        return;
    }
    ESP_LOGI(TAG, "%s connected", proto_name(params.proto));

    const hid_host_device_config_t dev_config = {
        .callback = hid_host_interface_callback,
        .callback_arg = NULL,
    };
    esp_err_t err = hid_host_device_open(handle, &dev_config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "cannot open %s: %s", proto_name(params.proto), esp_err_to_name(err));
        return;
    }

    // The device is open now, so every later failure must close it before
    // bailing out, or the handle leaks and the port never re-enumerates cleanly.
    if (params.sub_class == HID_SUBCLASS_BOOT_INTERFACE) {
        err = hid_class_request_set_protocol(handle, HID_REPORT_PROTOCOL_BOOT);
        if (err == ESP_OK && params.proto == HID_PROTOCOL_KEYBOARD) {
            err = hid_class_request_set_idle(handle, 0, 0);
        }
    } else {
        ESP_LOGW(TAG, "device has no boot interface; we only decode boot reports");
    }

    if (err == ESP_OK) {
        err = hid_host_device_start(handle);
    }

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "cannot start %s: %s", proto_name(params.proto), esp_err_to_name(err));
        hid_host_device_close(handle);
    }
}

// Runs in the HID host driver's background task — keep it light: just forward
// the connection event to our event task.
static void hid_host_device_callback(hid_host_device_handle_t handle,
                                     const hid_host_driver_event_t event,
                                     void *arg)
{
    (void)arg;
    const app_event_t evt = { .handle = handle, .event = event };
    xQueueSend(app_event_queue, &evt, 0);
}

// ---------------------------------------------------------------- tasks

// Owns the USB host library: installs it, then pumps its event loop forever.
static void usb_lib_task(void *arg)
{
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_config));
    xTaskNotifyGive((TaskHandle_t)arg);   // tell usb_input_start the host is up

    while (true) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            ESP_ERROR_CHECK(usb_host_device_free_all());
        }
    }
}

// Drains the connection-event queue and opens each new device.
static void usb_events_task(void *arg)
{
    (void)arg;
    app_event_t evt;
    while (true) {
        if (xQueueReceive(app_event_queue, &evt, portMAX_DELAY)) {
            if (evt.event == HID_HOST_DRIVER_EVENT_CONNECTED) {
                handle_device_connected(evt.handle);
            }
        }
    }
}

esp_err_t usb_input_start(usb_mouse_report_cb on_mouse, usb_keyboard_report_cb on_keyboard)
{
    s_on_mouse = on_mouse;
    s_on_keyboard = on_keyboard;

    app_event_queue = xQueueCreate(10, sizeof(app_event_t));
    if (app_event_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    BaseType_t created = xTaskCreatePinnedToCore(
        usb_lib_task, "usb_lib", 4096, xTaskGetCurrentTaskHandle(), 2, NULL, 0);
    if (created != pdTRUE) {
        return ESP_ERR_NO_MEM;
    }
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

    created = xTaskCreate(usb_events_task, "usb_events", 4096, NULL, 3, NULL);
    if (created != pdTRUE) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void usb_input_stats(uint32_t *mouse_reports, uint32_t *kbd_reports, uint32_t *xfer_errors)
{
    if (mouse_reports) *mouse_reports = s_mouse_reports;
    if (kbd_reports)   *kbd_reports   = s_kbd_reports;
    if (xfer_errors)   *xfer_errors   = s_xfer_errors;
}
