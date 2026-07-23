#include "diag.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"

#include "ble_hid.h"

static const char *TAG = "diag";

// A single BLE notification never takes more than microseconds, so if one has been
// in flight this long the send path is wedged, not busy — reboot so the device
// recovers on its own instead of sitting dead until it is physically reset. Set far
// above any legitimate send, so there are no false reboots (idle links report 0).
#define SEND_STUCK_REBOOT_MS   3000

#define TICK_MS                1000
#define HEAP_LOG_EVERY_TICKS   60     // ~60 s between resource-watermark lines

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

static void log_watermarks(void)
{
    ESP_LOGI(TAG, "heap: free=%u min-free=%u largest-block=%u | mbuf-fails=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
             (unsigned)ble_hid_mbuf_fail_count());
}

// A blocked send leaves its own (HID-callback) task un-runnable, so this task at a
// modest priority still gets to run and reboot us. A high-priority *spinner* would
// starve this task instead — but that case is already covered by the idle-task
// watchdog, so we needn't out-prioritise it here.
static void diag_task(void *arg)
{
    (void)arg;
    log_watermarks();   // a baseline line right after boot
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

        if (++tick >= HEAP_LOG_EVERY_TICKS) {
            tick = 0;
            log_watermarks();
        }
    }
}

void diag_start(void)
{
    if (xTaskCreate(diag_task, "diag", 3072, NULL, 4, NULL) != pdPASS) {
        ESP_LOGW(TAG, "diag task not started (out of memory)");
    }
}
