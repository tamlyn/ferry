// USB host input: enumerate a mouse and/or keyboard (directly or through a hub)
// and deliver decoded HID reports to the app via callbacks.
//
// The keyboard is driven in the HID *boot* protocol (modifiers + up to six
// keycodes) — all we need, and universally supported. The mouse is driven in
// *report* protocol: its own report descriptor is parsed (see mouse_fmt_parse)
// to locate the button, X/Y, wheel and horizontal-pan fields, so extra buttons
// (back/forward/gesture) and horizontal scroll come through — none of which the
// 3-button boot report can carry. A mouse whose descriptor we cannot parse falls
// back to the boot report (basic buttons + motion + wheel), so any mouse still
// works.

#include "usb_input.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

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

// The open interface handles of the mouse and keyboard, stashed so the USB
// watchdog (diag.c) can issue a liveness probe when reports go silent. NULL when
// no such device is open. Guarded by s_probe_lock: the probe holds the lock
// across its (≤~5 s) control request, and the disconnect handler takes it before
// clearing+closing a handle — so a disconnect landing mid-probe waits rather than
// closing a handle out from under an in-flight transfer.
static hid_host_device_handle_t s_mouse_handle = NULL;
static hid_host_device_handle_t s_kbd_handle   = NULL;
static SemaphoreHandle_t        s_probe_lock    = NULL;

static const char *proto_name(uint8_t proto)
{
    switch (proto) {
    case HID_PROTOCOL_KEYBOARD: return "keyboard";
    case HID_PROTOCOL_MOUSE:    return "mouse";
    default:                    return "generic HID";
    }
}

// -------------------------------------------- report-descriptor parsing

// One field's location within a report body (bits, after any report-ID byte).
typedef struct {
    uint16_t bit_off;
    uint8_t  bits;
    bool     present;
} field_t;

// Where the fields we care about live in a mouse's input report.
typedef struct {
    bool     valid;
    bool     has_id;      // report is prefixed by a 1-byte report ID
    uint8_t  report_id;
    field_t  buttons, x, y, wheel, pan;
    uint16_t min_len;     // shortest report body we can decode (bytes)
} mouse_fmt_t;

// Walk a HID report descriptor and locate the mouse input report's fields.
//
// This is a deliberately small parser: it tracks just enough item state (usage
// page, report size/count/id, and the current usage list) to assign each Input
// field a bit offset, then picks the report ID that carries both X and Y as the
// mouse report and records its button/X/Y/wheel/AC-pan fields. Fields that are
// not byte-aligned (rare on real mice) are skipped, and a descriptor with no
// X/Y returns false so the caller falls back to the boot report.
static bool mouse_fmt_parse(const uint8_t *d, size_t n, mouse_fmt_t *out)
{
    memset(out, 0, sizeof(*out));

    uint16_t usage_page = 0;
    uint32_t rsize = 0, rcount = 0;
    int      rid = 0;              // current report ID (0 = none declared)
    bool     any_id = false;
    uint32_t in_bit = 0;          // input-report bit offset within the current ID

    uint32_t usages[16];
    int      n_usages = 0;
    uint32_t umin = 0;
    bool     has_range = false;

    enum { K_BTN, K_X, K_Y, K_WHEEL, K_PAN };
    struct { int rid; uint32_t off; uint8_t bits; int kind; } cand[24];
    int nc = 0;

    size_t i = 0;
    while (i < n) {
        uint8_t p = d[i++];
        if (p == 0xFE) {                       // long item — skip its payload
            if (i >= n) break;
            uint8_t dsize = d[i++];
            i += (size_t)dsize + 1;
            continue;
        }
        uint8_t bsize = p & 0x03;
        if (bsize == 3) bsize = 4;
        uint8_t btype = (p >> 2) & 0x03;
        uint8_t btag  = (p >> 4) & 0x0F;
        uint32_t val = 0;
        for (int k = 0; k < bsize && i < n; k++) {
            val |= (uint32_t)d[i++] << (8 * k);
        }

        if (btype == 1) {                      // Global
            switch (btag) {
            case 0x0: usage_page = (uint16_t)val; break;
            case 0x7: rsize = val; break;
            case 0x8: rid = (int)val; any_id = true; in_bit = 0; break;
            case 0x9: rcount = val; break;
            default: break;
            }
        } else if (btype == 2) {               // Local
            switch (btag) {
            case 0x0: if (n_usages < 16) usages[n_usages++] = val; break;
            case 0x1: umin = val; has_range = true; break;
            case 0x2: has_range = true; break;   // usage max: only the min matters here
            default: break;
            }
        } else if (btype == 0) {               // Main
            if (btag == 0x8) {                 // Input
                if (usage_page == 0x09) {      // Button page — a button bitmap
                    if (nc < 24) {
                        cand[nc].rid = rid; cand[nc].off = in_bit;
                        cand[nc].bits = (uint8_t)(rsize * rcount); cand[nc].kind = K_BTN;
                        nc++;
                    }
                } else {
                    for (uint32_t f = 0; f < rcount; f++) {
                        uint32_t usage = (f < (uint32_t)n_usages) ? usages[f]
                                       : (n_usages ? usages[n_usages - 1]
                                       : (has_range ? umin + f : 0));
                        int kind = -1;
                        if (usage_page == 0x01) {
                            if (usage == 0x30) kind = K_X;
                            else if (usage == 0x31) kind = K_Y;
                            else if (usage == 0x38) kind = K_WHEEL;
                        } else if (usage_page == 0x0C && usage == 0x0238) {
                            kind = K_PAN;
                        }
                        if (kind >= 0 && nc < 24) {
                            cand[nc].rid = rid; cand[nc].off = in_bit + f * rsize;
                            cand[nc].bits = (uint8_t)rsize; cand[nc].kind = kind;
                            nc++;
                        }
                    }
                }
                in_bit += rsize * rcount;
            }
            // Every main item ends the current set of locals. (Output/Feature
            // items live in separate reports, so they don't move in_bit.)
            n_usages = 0; umin = 0; has_range = false;
        }
    }

    // The mouse report is the one carrying both X and Y.
    int mouse_rid = -1;
    for (int a = 0; a < nc && mouse_rid < 0; a++) {
        if (cand[a].kind != K_X) continue;
        for (int b = 0; b < nc; b++) {
            if (cand[b].kind == K_Y && cand[b].rid == cand[a].rid) {
                mouse_rid = cand[a].rid;
                break;
            }
        }
    }
    if (mouse_rid < 0) return false;

    out->has_id = any_id;
    out->report_id = (uint8_t)mouse_rid;
    uint32_t min_bits = 0;
    for (int a = 0; a < nc; a++) {
        if (cand[a].rid != mouse_rid) continue;
        field_t f = { (uint16_t)cand[a].off, cand[a].bits, true };
        // Byte-aligned fields only, so decode can read whole bytes. Buttons need
        // only an aligned start (they are read as up to two bytes).
        if (f.bit_off & 7) continue;
        if (cand[a].kind != K_BTN && (f.bits & 7)) continue;
        switch (cand[a].kind) {
        case K_BTN:   out->buttons = f; break;
        case K_X:     out->x = f;       break;
        case K_Y:     out->y = f;       break;
        case K_WHEEL: out->wheel = f;   break;
        case K_PAN:   out->pan = f;     break;
        }
        uint32_t end = (uint32_t)cand[a].off + cand[a].bits;
        if (end > min_bits) min_bits = end;
    }
    out->min_len = (uint16_t)((min_bits + 7) / 8);
    out->valid = out->x.present && out->y.present;
    return out->valid;
}

// ---------------------------------------- per-interface decode selection

typedef enum { DEV_KBD, DEV_MOUSE_BOOT, DEV_MOUSE_REPORT } dev_kind_t;

typedef struct {
    bool                     in_use;
    hid_host_device_handle_t handle;
    dev_kind_t               kind;
    mouse_fmt_t              fmt;   // valid when kind == DEV_MOUSE_REPORT
} hid_iface_t;

#define MAX_HID_IFACES 6
static hid_iface_t s_ifaces[MAX_HID_IFACES];

static hid_iface_t *iface_find(hid_host_device_handle_t h)
{
    for (int i = 0; i < MAX_HID_IFACES; i++) {
        if (s_ifaces[i].in_use && s_ifaces[i].handle == h) {
            return &s_ifaces[i];
        }
    }
    return NULL;
}

static hid_iface_t *iface_add(hid_host_device_handle_t h)
{
    hid_iface_t *d = iface_find(h);
    if (d) return d;
    for (int i = 0; i < MAX_HID_IFACES; i++) {
        if (!s_ifaces[i].in_use) {
            s_ifaces[i] = (hid_iface_t){ .in_use = true, .handle = h };
            return &s_ifaces[i];
        }
    }
    return NULL;
}

static void iface_remove(hid_host_device_handle_t h)
{
    hid_iface_t *d = iface_find(h);
    if (d) *d = (hid_iface_t){0};
}

// ---------------------------------------------------------------- decoding

static int field_signed(const uint8_t *body, field_t f)
{
    if (!f.present) return 0;
    uint16_t b = f.bit_off >> 3;
    if (f.bits <= 8) {
        return (int8_t)body[b];
    }
    return (int16_t)(body[b] | ((uint16_t)body[b + 1] << 8));
}

static uint16_t field_unsigned(const uint8_t *body, field_t f)
{
    if (!f.present) return 0;
    uint16_t b = f.bit_off >> 3;
    uint16_t v = body[b];
    if (f.bits > 8) v |= (uint16_t)body[b + 1] << 8;
    if (f.bits < 16) v &= (uint16_t)((1u << f.bits) - 1);
    return v;
}

// Decode a report-protocol mouse report by the parsed field layout.
static void dispatch_mouse_report(const uint8_t *data, size_t len, const mouse_fmt_t *f)
{
    if (s_on_mouse == NULL) return;

    const uint8_t *body = data;
    if (f->has_id) {
        // The interface multiplexes several reports (mouse, consumer, system) by
        // ID; ignore everything but the mouse report.
        if (len == 0 || data[0] != f->report_id) return;
        body = data + 1;
        len -= 1;
    }
    if (len < f->min_len) return;

    uint8_t buttons = (uint8_t)field_unsigned(body, f->buttons);
    int dx    = field_signed(body, f->x);
    int dy    = field_signed(body, f->y);
    int wheel = field_signed(body, f->wheel);
    int pan   = field_signed(body, f->pan);
    s_on_mouse(buttons, dx, dy, wheel, pan);
}

// Decode a boot-protocol mouse report: 3 buttons + relative dx/dy, plus the
// signed wheel byte nearly every mouse appends as a 4th byte.
static void dispatch_mouse_boot(const uint8_t *data, size_t len)
{
    if (len < sizeof(hid_mouse_input_report_boot_t) || s_on_mouse == NULL) {
        return;
    }
    const hid_mouse_input_report_boot_t *r =
        (const hid_mouse_input_report_boot_t *)data;
    uint8_t buttons = (r->buttons.button1 ? 0x01 : 0) |
                      (r->buttons.button2 ? 0x02 : 0) |
                      (r->buttons.button3 ? 0x04 : 0);
    int wheel = (len > sizeof(hid_mouse_input_report_boot_t))
                    ? (int8_t)data[sizeof(hid_mouse_input_report_boot_t)]
                    : 0;
    s_on_mouse(buttons, r->x_displacement, r->y_displacement, wheel, 0);
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

        if (params.proto == HID_PROTOCOL_MOUSE)         s_mouse_reports++;
        else if (params.proto == HID_PROTOCOL_KEYBOARD) s_kbd_reports++;

        hid_iface_t *d = iface_find(handle);
        if (d == NULL) {
            break;
        }
        switch (d->kind) {
        case DEV_MOUSE_REPORT: dispatch_mouse_report(data, len, &d->fmt); break;
        case DEV_MOUSE_BOOT:   dispatch_mouse_boot(data, len);            break;
        case DEV_KBD:          dispatch_keyboard_report(data, len);       break;
        }
        break;
    }
    case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "%s disconnected", proto_name(params.proto));
        iface_remove(handle);
        // Drop the stashed probe handle before closing so a concurrent probe can't
        // touch a closed handle. Taking the lock waits out any in-flight probe.
        xSemaphoreTake(s_probe_lock, portMAX_DELAY);
        if (handle == s_mouse_handle) s_mouse_handle = NULL;
        if (handle == s_kbd_handle)   s_kbd_handle = NULL;
        ESP_ERROR_CHECK(hid_host_device_close(handle));
        xSemaphoreGive(s_probe_lock);
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

// A new device was connected. We only host mice and keyboards, so ignore every
// other HID interface — a keyboard's extra media-key interface, a mouse receiver's
// vendor interface, a joystick. Besides being undecodable here, claiming them ties
// up the ESP32-S3's limited USB host channels on pipes we never read, which is
// exactly what starves a second device sharing a hub.
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
    dev_kind_t kind;
    mouse_fmt_t fmt = {0};
    if (params.proto == HID_PROTOCOL_MOUSE) {
        // Report protocol + descriptor parse gets the extra buttons and
        // horizontal scroll; a descriptor we cannot parse falls back to boot.
        err = hid_class_request_set_protocol(handle, HID_REPORT_PROTOCOL_REPORT);
        size_t desc_len = 0;
        uint8_t *desc = (err == ESP_OK) ? hid_host_get_report_descriptor(handle, &desc_len) : NULL;
        if (desc != NULL && mouse_fmt_parse(desc, desc_len, &fmt)) {
            kind = DEV_MOUSE_REPORT;
            ESP_LOGI(TAG, "mouse report id=%d: buttons@%d(%db) x@%d y@%d wheel@%d pan@%d",
                     fmt.has_id ? fmt.report_id : 0,
                     fmt.buttons.bit_off, fmt.buttons.bits,
                     fmt.x.bit_off, fmt.y.bit_off,
                     fmt.wheel.present ? fmt.wheel.bit_off : -1,
                     fmt.pan.present ? fmt.pan.bit_off : -1);
        } else {
            ESP_LOGW(TAG, "mouse descriptor not parsed; using boot report");
            err = hid_class_request_set_protocol(handle, HID_REPORT_PROTOCOL_BOOT);
            kind = DEV_MOUSE_BOOT;
        }
    } else {
        err = hid_class_request_set_protocol(handle, HID_REPORT_PROTOCOL_BOOT);
        if (err == ESP_OK) {
            err = hid_class_request_set_idle(handle, 0, 0);
        }
        kind = DEV_KBD;
    }

    if (err == ESP_OK) {
        err = hid_host_device_start(handle);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "cannot start %s: %s", proto_name(params.proto), esp_err_to_name(err));
        hid_host_device_close(handle);
        return;
    }

    hid_iface_t *d = iface_add(handle);
    if (d == NULL) {
        ESP_LOGW(TAG, "no free device slot; dropping %s", proto_name(params.proto));
        hid_host_device_close(handle);
        return;
    }
    d->kind = kind;
    d->fmt = fmt;

    // Fully open and streaming: remember the handle so the watchdog can probe it.
    xSemaphoreTake(s_probe_lock, portMAX_DELAY);
    if (params.proto == HID_PROTOCOL_MOUSE) {
        s_mouse_handle = handle;
    } else if (params.proto == HID_PROTOCOL_KEYBOARD) {
        s_kbd_handle = handle;
    }
    xSemaphoreGive(s_probe_lock);
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

    s_probe_lock = xSemaphoreCreateMutex();
    if (s_probe_lock == NULL) {
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

bool usb_input_probe_alive(void)
{
    xSemaphoreTake(s_probe_lock, portMAX_DELAY);

    // Prefer the mouse (the device the freeze silences during active use), but a
    // keyboard-only setup is probed just as well.
    hid_host_device_handle_t handle = s_mouse_handle ? s_mouse_handle : s_kbd_handle;
    if (handle == NULL) {
        // Nothing open to test — silence here is a genuinely idle/absent device,
        // not a wedged controller. Report alive so the watchdog holds its fire.
        xSemaphoreGive(s_probe_lock);
        return true;
    }

    // GET_PROTOCOL is a mandatory, read-only HID class request every boot device
    // answers in milliseconds. A wedged USB controller never completes the control
    // transfer, so hid_control_transfer()'s 5 s wait expires and we get a timeout —
    // the signal the watchdog acts on.
    hid_report_protocol_t proto;
    esp_err_t err = hid_class_request_get_protocol(handle, &proto);
    xSemaphoreGive(s_probe_lock);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "usb probe: alive");
        return true;
    }
    ESP_LOGW(TAG, "usb probe: TIMEOUT (%s)", esp_err_to_name(err));
    return false;
}
