#ifndef BLE_HID_H
#define BLE_HID_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

// Bring up the BLE HID-over-GATT peripheral: a combined absolute-pointer mouse +
// boot keyboard the host can pair with and drive with no host-side software.
// Advertises, bonds (Just Works — the device has no keypad), and persists bonds
// in NVS. Call nvs_flash_init() before this.
esp_err_t ble_hid_init(void);

// True once a host is connected *and* the link is encrypted — i.e. it is safe to
// send input reports. Sends before this are dropped.
bool ble_hid_ready(void);

// Send an absolute-pointer report. buttons is a bitmask (bit0 = left, bit1 =
// right, bit2 = middle...); x/y are absolute coordinates in 0..ABS_AXIS_MAX.
esp_err_t ble_hid_send_mouse(uint8_t buttons, uint16_t x, uint16_t y);

// Send a boot-keyboard report: a modifier bitmask plus up to six concurrent
// keycodes (0 = unused slot).
esp_err_t ble_hid_send_keyboard(uint8_t modifiers, const uint8_t keys[6]);

#endif
