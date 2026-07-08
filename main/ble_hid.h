#ifndef BLE_HID_H
#define BLE_HID_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

// The number of hosts we can hold live at once — a two-machine KVM. Slot indices
// are 0..BLE_HID_MAX_HOSTS-1; by convention slot 0 is the "left" host and slot 1
// the "right", in connection order (see kvm.c).
#define BLE_HID_MAX_HOSTS 2

// Bring up the BLE HID-over-GATT peripheral on the NimBLE host stack: a combined
// absolute-pointer mouse + boot keyboard that up to BLE_HID_MAX_HOSTS computers
// can pair with and drive with no host-side software. Advertises (and keeps
// advertising while a slot is free), bonds (Just Works — the device has no
// keypad), keeps notification state per connection, and persists bonds in NVS.
// Call nvs_flash_init() before this.
esp_err_t ble_hid_init(void);

// True once host `host` is connected *and* has subscribed to our input reports —
// i.e. it is safe to send it input. Sends to a host that is not ready are
// dropped. `host` out of range returns false.
bool ble_hid_ready(int host);

// Send an absolute-pointer report to host `host`. buttons is a bitmask (bit0 =
// left, bit1 = right, bit2 = middle...); x/y are absolute coordinates in
// 0..ABS_AXIS_MAX; wheel is a signed relative scroll delta. NimBLE only puts the
// notification on the wire if that host subscribed, so input never leaks to the
// wrong machine.
esp_err_t ble_hid_send_mouse(int host, uint8_t buttons, uint16_t x, uint16_t y, int8_t wheel);

// Send a *relative* pointer report (report id 3) to host `host` — a signed dx/dy
// delta, the only motion that crosses a display boundary that absolute positioning
// is clamped within. Motion only: buttons ride the absolute report, and report 3
// must not carry any (see our_descriptor.c). Dropped if the host hasn't subscribed
// to the relative report. The KVM uses it to walk the cursor onto an adjacent
// display, after which absolute positioning re-sticks there.
esp_err_t ble_hid_send_mouse_rel(int host, int8_t dx, int8_t dy);

// Send a boot-keyboard report to host `host`: a modifier bitmask plus up to six
// concurrent keycodes (0 = unused slot).
esp_err_t ble_hid_send_keyboard(int host, uint8_t modifiers, const uint8_t keys[6]);

#endif
