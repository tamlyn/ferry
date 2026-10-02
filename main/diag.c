#include "diag.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "ble_hid.h"
#include "usb_input.h"

static const char *TAG = "diag";

// Why the *previous* boot ended, carried across the reset in RTC memory — which
// survives esp_restart() but holds garbage after a power cycle, hence the magic.
// esp_reset_reason() only ever says "software" for our two watchdogs, and which one
// pulled the trigger is exactly the part we need; a freeze almost never happens
// while a serial capture is attached to catch the log line as it goes out.
#define CRUMB_MAGIC  0x46525259u   // 'FRRY'

typedef enum {
    CRUMB_USB_WEDGED = 1,
    CRUMB_BLE_SEND_WEDGED,
} crumb_reason_t;

typedef struct {
    uint32_t magic;
    uint32_t reason;
    uint32_t detail_ms;   // how long the wedge had lasted when we gave up on it
    uint32_t uptime_ms;   // how long the board had been running
} crumb_t;

static RTC_NOINIT_ATTR crumb_t s_crumb;

static void reboot_leaving_crumb(crumb_reason_t reason, uint32_t detail_ms)
{
    s_crumb.magic     = CRUMB_MAGIC;
    s_crumb.reason    = reason;
    s_crumb.detail_ms = detail_ms;
    s_crumb.uptime_ms = (uint32_t)(esp_timer_get_time() / 1000);
    vTaskDelay(pdMS_TO_TICKS(50));   // let the line clear UART0 first
    esp_restart();
}

// A single BLE notification never takes more than microseconds, so if one has been
// in flight this long the send path is wedged, not busy — reboot so the device
// recovers on its own instead of sitting dead until it is physically reset. Set far
// above any legitimate send, so there are no false reboots (idle links report 0).
#define SEND_STUCK_REBOOT_MS   3000

#define TICK_MS                1000
#define HEARTBEAT_EVERY_TICKS  15     // ~15 s between activity/resource lines

// USB-input stall watchdog (see AGENTS.md freeze notes + plan). The ESP-IDF USB
// host controller's interrupt handler silently wedges after a while, so HID reports
// stop arriving — mouse and keyboard together (they share the one controller). A
// full re-init via esp_restart() recovers it. We can't reboot on report-silence
// alone (a genuinely idle mouse looks identical), so once reports go quiet we
// actively probe a connected device with a GET_PROTOCOL request: a healthy device
// answers in ms, a wedged controller doesn't.
//
// Confirmation is over *elapsed time*, not just a count of failures, because
// consecutive failures are not independent evidence. A probe that times out leaves
// its control transfer outstanding, so the next one cannot even be submitted and
// fails whatever the controller's true state — two failures in quick succession are
// one observation wearing two hats. Demanding that the failures also span
// FAIL_WINDOW_MS is what makes them count: a transient stall clears and answers the
// next probe, a wedged controller keeps failing for as long as we care to ask.
// (Before this the interval was stamped *before* the blocking probe, so its 5 s gap
// collapsed to the 1 s tick and a single timeout rebooted the device ~1 s later.)
//
// The window is kept short because the whole of it is freeze the user sits through,
// while the reboot it guards against costs 1.4 s to both hosts reconnected — and no
// stall has ever answered a later probe (`usb probe recovered` has yet to fire). Two
// failures 5 s apart is thin evidence by design: the cost of being wrong is a 1.4 s
// reboot of an already-dead desk.
#define T_SILENCE_MS           3000   // reports quiet this long before we start probing
#define PROBE_INTERVAL_MS      5000   // gap between probes, measured from the last one finishing
#define PROBE_FAILS_TO_REBOOT     2   // failures needed...
#define FAIL_WINDOW_MS         5000   // ...and the span they must cover, before we call it wedged

// A desk nobody is sitting at needs no fast probing, and every probe is a control
// transfer injected alongside the HID interrupt pipes on a controller with a known
// wedging bug — so back right off once silence stops looking like a pause in use.
#define IDLE_AFTER_MS         30000
#define IDLE_PROBE_INTERVAL_MS 30000

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

    if (s_crumb.magic == CRUMB_MAGIC) {
        const char *what = s_crumb.reason == CRUMB_USB_WEDGED      ? "USB input wedged"
                         : s_crumb.reason == CRUMB_BLE_SEND_WEDGED ? "BLE send wedged"
                                                                   : "unrecognised watchdog";
        ESP_LOGW(TAG, "previous boot ended: %s after %ums, having run %us  <-- freeze",
                 what, (unsigned)s_crumb.detail_ms, (unsigned)(s_crumb.uptime_ms / 1000));
        // Cleared once reported, so a later reset that happens to leave RTC memory
        // intact (the EN pin, a serial host waggling RTS) can't re-serve a stale
        // crumb as fresh news.
        s_crumb.magic = 0;
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
            reboot_leaving_crumb(CRUMB_BLE_SEND_WEDGED, stuck);
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

// Kept separate from diag_task because its liveness probe blocks up to ~5 s on a
// wedged controller; running it here means that block never delays the send-wedge
// check. A BLE host must be subscribed before we probe or reboot: with no host
// there is nothing driving input and nothing to recover, so silence is expected —
// gating on ble_hid_ready also means a fresh boot with no reconnected host doesn't
// probe or reboot-loop.
static void usb_watchdog_task(void *arg)
{
    (void)arg;
    uint32_t prev_reports = 0;
    {
        uint32_t mouse = 0, kbd = 0;
        usb_input_stats(&mouse, &kbd, NULL);
        prev_reports = mouse + kbd;
    }

    TickType_t last_activity = xTaskGetTickCount();
    TickType_t last_probe    = 0;
    TickType_t first_fail    = 0;
    int fail_count = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
        TickType_t now = xTaskGetTickCount();

        uint32_t mouse = 0, kbd = 0;
        usb_input_stats(&mouse, &kbd, NULL);
        uint32_t reports = mouse + kbd;
        if (reports != prev_reports) {
            prev_reports = reports;
            last_activity = now;
            fail_count = 0;
            continue;
        }

        if (now - last_activity < pdMS_TO_TICKS(T_SILENCE_MS)) {
            continue;
        }
        if (!ble_hid_ready(0) && !ble_hid_ready(1)) {
            continue;
        }
        // Backing off only holds while nothing looks wrong. A controller that wedges
        // during a long idle spell still looks idle when its owner comes back — the
        // mouse they are moving produces no reports — so once a probe has failed we
        // return to the fast cadence rather than confirming at 30 s a go.
        bool idle = fail_count == 0 && (now - last_activity) >= pdMS_TO_TICKS(IDLE_AFTER_MS);
        if (now - last_probe < pdMS_TO_TICKS(idle ? IDLE_PROBE_INTERVAL_MS
                                                  : PROBE_INTERVAL_MS)) {
            continue;
        }

        bool alive = usb_input_probe_alive();
        last_probe = xTaskGetTickCount();   // on completion: the probe blocks up to ~5 s

        if (alive) {
            // Whether a stall ever heals on its own is the number that sets
            // FAIL_WINDOW_MS: the wait is pure freeze time (the reboot itself costs
            // ~1.5 s), so it is only worth paying if stalls do sometimes clear.
            if (fail_count > 0) {
                ESP_LOGW(TAG, "usb probe recovered after %d failure(s) over %ums — stall was transient",
                         fail_count, (unsigned)pdTICKS_TO_MS(last_probe - first_fail));
            }
            fail_count = 0;
            continue;
        }
        if (fail_count++ == 0) {
            first_fail = last_probe;
        }
        TickType_t failing_for = last_probe - first_fail;
        if (fail_count >= PROBE_FAILS_TO_REBOOT && failing_for >= pdMS_TO_TICKS(FAIL_WINDOW_MS)) {
            ESP_LOGE(TAG, "USB input wedged (%d probes failed over %ums) — rebooting to re-enumerate",
                     fail_count, (unsigned)pdTICKS_TO_MS(failing_for));
            reboot_leaving_crumb(CRUMB_USB_WEDGED, pdTICKS_TO_MS(failing_for));
        }
    }
}

void diag_start(void)
{
    if (xTaskCreate(diag_task, "diag", 3072, NULL, 4, NULL) != pdPASS) {
        ESP_LOGW(TAG, "diag task not started (out of memory)");
    }
    if (xTaskCreate(usb_watchdog_task, "usb_wdt", 3072, NULL, 4, NULL) != pdPASS) {
        ESP_LOGW(TAG, "usb watchdog task not started (out of memory)");
    }
}
