#ifndef USB_INPUT_H
#define USB_INPUT_H

#include <stdint.h>

#include "esp_err.h"

// Decoded HID input, delivered from the USB host driver's task. Keep handlers
// light — they run on that task.
//
// buttons is a bitmask (bit0 = left, bit1 = right, bit2 = middle); dx/dy are
// relative motion counts; wheel is the signed scroll delta (0 when the mouse
// sent no wheel byte). keys holds up to six concurrent keycodes (0 = unused).
typedef void (*usb_mouse_report_cb)(uint8_t buttons, int dx, int dy, int wheel);
typedef void (*usb_keyboard_report_cb)(uint8_t modifiers, const uint8_t keys[6]);

// Start hosting USB HID: install the USB host + HID class driver and enumerate a
// boot-protocol mouse and/or keyboard (directly or through a hub). Decoded
// reports are delivered to the callbacks. Either callback may be NULL.
esp_err_t usb_input_start(usb_mouse_report_cb on_mouse, usb_keyboard_report_cb on_keyboard);

#endif
