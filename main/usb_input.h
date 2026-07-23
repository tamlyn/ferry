#ifndef USB_INPUT_H
#define USB_INPUT_H

#include <stdint.h>

#include "esp_err.h"

// Decoded HID input, delivered from the USB host driver's task. Keep handlers
// light — they run on that task.
//
// buttons is a bitmask (bit0 = left, bit1 = right, bit2 = middle, bit3 = back,
// bit4 = forward, bit5+ = extra); dx/dy are relative motion counts; wheel is the
// signed vertical scroll delta and pan the signed horizontal one (both 0 when
// the mouse sent none). keys holds up to six concurrent keycodes (0 = unused).
typedef void (*usb_mouse_report_cb)(uint8_t buttons, int dx, int dy, int wheel, int pan);
typedef void (*usb_keyboard_report_cb)(uint8_t modifiers, const uint8_t keys[6]);

// Start hosting USB HID: install the USB host + HID class driver and enumerate a
// boot-protocol mouse and/or keyboard (directly or through a hub). Decoded
// reports are delivered to the callbacks. Either callback may be NULL.
esp_err_t usb_input_start(usb_mouse_report_cb on_mouse, usb_keyboard_report_cb on_keyboard);

// Diagnostics: cumulative counts of HID reports received from the USB host driver
// (per device) and transfer errors, since boot. Any out-param may be NULL. Used by
// diag.c to tell a wedged USB input path (counts flat during a freeze) from a
// downstream BLE stall (counts still climbing).
void usb_input_stats(uint32_t *mouse_reports, uint32_t *kbd_reports, uint32_t *xfer_errors);

#endif
