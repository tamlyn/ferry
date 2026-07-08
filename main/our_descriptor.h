#ifndef OUR_DESCRIPTOR_H
#define OUR_DESCRIPTOR_H

#include <stdint.h>

// Report IDs used in the combined HID report map. The host addresses each of our
// reports by these IDs; they also tag the report-reference descriptors NimBLE
// builds, which is how a host maps a notification back to mouse vs keyboard.
#define REPORT_ID_MOUSE     1
#define REPORT_ID_KEYBOARD  2

// A second, *relative* pointer used to walk the cursor across a display
// boundary. Absolute X/Y is clamped by the host to the display the cursor is
// currently on; relative motion is the only thing that crosses onto an adjacent
// display, after which absolute positioning re-sticks there. This is how the KVM
// hops between two displays on one host (see kvm.c). It lives in its own HID
// service instance and carries no buttons — see our_descriptor.c for the two
// macOS behaviours that force exactly that shape.
#define REPORT_ID_MOUSE_REL 3

// Payload sizes (excluding the report ID, which the GATT report characteristic
// carries out of band).
#define MOUSE_REPORT_SIZE     6   // 1 button byte + X(16) + Y(16) + wheel(8)
#define MOUSE_REL_REPORT_SIZE 2   // relX(8) + relY(8) — no buttons, by design
#define KEYBOARD_REPORT_SIZE  8   // modifiers + reserved + 6 keycodes

// The cursor lives in an absolute coordinate space of 0..ABS_AXIS_MAX on both
// axes. This is the whole point of the project: the host treats X/Y as an
// absolute position, not a relative delta, so we can place the cursor anywhere
// (and later "hop" it between machines).
#define ABS_AXIS_MAX  0x7FFF   // 32767

// Mouse + keyboard map (HID service instance one).
extern const uint8_t our_report_descriptor[];
extern const uint16_t our_report_descriptor_length;

// Relative-pointer map (HID service instance two).
extern const uint8_t our_rel_report_descriptor[];
extern const uint16_t our_rel_report_descriptor_length;

#endif
