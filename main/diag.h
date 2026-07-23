#ifndef DIAG_H
#define DIAG_H

// Diagnostics for the intermittent freeze. Two jobs:
//
//  - Log *why* the last boot happened (esp_reset_reason), so an auto-reboot from a
//    panic, brownout, or watchdog leaves a trace in the console instead of
//    vanishing. (Panics already reboot in 0s here, so a crash self-recovers and
//    would otherwise go unnoticed.)
//
//  - Run a monitor task that reboots the board if a BLE send wedges for seconds —
//    the silent hang the task-WDT can't see, because a *blocked* task doesn't
//    starve the idle task the WDT monitors — and periodically logs the heap and
//    mbuf-failure watermarks so a slow leak shows up before it kills the device.

// Log the reason the chip last reset. Call once, early in app_main.
void diag_log_reset_reason(void);

// Start the background monitor task (stuck-send reboot + resource watermarks).
void diag_start(void);

#endif
