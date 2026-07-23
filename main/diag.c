#include "diag.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_system.h"

#include "ble_hid.h"
#include "usb_input.h"

static const char *TAG = "diag";

// A single BLE notification never takes more than microseconds, so if one has been
// in flight this long the send path is wedged, not busy — reboot so the device
// recovers on its own instead of sitting dead until it is physically reset. Set far
// above any legitimate send, so there are no false reboots (idle links report 0).
#define SEND_STUCK_REBOOT_MS   3000

#define TICK_MS                1000
#define HEARTBEAT_EVERY_TICKS  15     // ~15 s between activity/resource lines

static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "external pin";
    case ESP_RST_SW:        return "software (esp_restart)";
    case ESP_RST_PANIC:     return "panic / exception";
    case ESP_RST_INT_WDT:   return "interrupt watchdog";
    case ESP_RST_TASK_WDT:  return "task watchdog";
    case ESP_RST_WDT:       return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "deep-sleep wake";
    case ESP_RST_BROWNOUT:  return "brownout (supply dip)";
    case ESP_RST_SDIO:      return "SDIO";
    case ESP_RST_USB:       return "USB peripheral";
    case ESP_RST_JTAG:      return "JTAG";
    default:                return "unknown";
    }
}

void diag_log_reset_reason(void)
{
    esp_reset_reason_t r = esp_reset_reason();
    // A fault reset is the interesting case for the freeze hunt — flag it loudly so
    // it stands out in a passive capture.
    bool fault = (r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT ||
                  r == ESP_RST_WDT || r == ESP_RST_BROWNOUT);
    if (fault) {
        ESP_LOGW(TAG, "last reset: %s (code %d)  <-- fault", reset_reason_str(r), r);
    } else {
        ESP_LOGI(TAG, "last reset: %s (code %d)", reset_reason_str(r), r);
    }
}

// One line that answers "where is a freeze stuck?" at a glance. Deltas since the
// previous line: usb mouse/kbd = reports the USB host handed us (flat during a
// freeze => USB input wedged); tx0/tx1 = per-host mouse sends attempt/ok (climbing
// while the cursor is frozen => the stall is on the wire, not upstream). Absolute
// heap + mbuf failures ride along to catch a slow leak.
static void log_heartbeat(uint32_t d_mouse, uint32_t d_kbd, uint32_t d_err,
                          uint32_t d_a0, uint32_t d_o0, uint32_t d_a1, uint32_t d_o1)
{
    ESP_LOGI(TAG,
             "usb: mouse+%u kbd+%u err+%u | tx0 +%u/+%u tx1 +%u/+%u | "
             "heap free=%u min=%u | mbuf-fails=%u",
             (unsigned)d_mouse, (unsigned)d_kbd, (unsigned)d_err,
             (unsigned)d_a0, (unsigned)d_o0, (unsigned)d_a1, (unsigned)d_o1,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size(),
             (unsigned)ble_hid_mbuf_fail_count());
}

// A blocked send leaves its own (HID-callback) task un-runnable, so this task at a
// modest priority still gets to run and reboot us. A high-priority *spinner* would
// starve this task instead — but that case is already covered by the idle-task
// watchdog, so we needn't out-prioritise it here.
static void diag_task(void *arg)
{
    (void)arg;
    uint32_t p_mouse = 0, p_kbd = 0, p_err = 0;
    uint32_t p_a0 = 0, p_o0 = 0, p_a1 = 0, p_o1 = 0;
    usb_input_stats(&p_mouse, &p_kbd, &p_err);
    ble_hid_tx_counts(0, &p_a0, &p_o0);
    ble_hid_tx_counts(1, &p_a1, &p_o1);

    unsigned tick = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));

        uint32_t stuck = ble_hid_send_stuck_ms();
        if (stuck >= SEND_STUCK_REBOOT_MS) {
            ESP_LOGE(TAG, "BLE send wedged %ums (free heap %u) — rebooting to recover",
                     (unsigned)stuck, (unsigned)esp_get_free_heap_size());
            vTaskDelay(pdMS_TO_TICKS(50));   // let the line clear UART0 first
            esp_restart();
        }

        if (++tick < HEARTBEAT_EVERY_TICKS) {
            continue;
        }
        tick = 0;

        uint32_t mouse, kbd, err, a0, o0, a1, o1;
        usb_input_stats(&mouse, &kbd, &err);
        ble_hid_tx_counts(0, &a0, &o0);
        ble_hid_tx_counts(1, &a1, &o1);
        log_heartbeat(mouse - p_mouse, kbd - p_kbd, err - p_err,
                      a0 - p_a0, o0 - p_o0, a1 - p_a1, o1 - p_o1);
        p_mouse = mouse; p_kbd = kbd; p_err = err;
        p_a0 = a0; p_o0 = o0; p_a1 = a1; p_o1 = o1;
    }
}

void diag_start(void)
{
    if (xTaskCreate(diag_task, "diag", 3072, NULL, 4, NULL) != pdPASS) {
        ESP_LOGW(TAG, "diag task not started (out of memory)");
    }
}
