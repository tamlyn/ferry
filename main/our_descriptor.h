#ifndef OUR_DESCRIPTOR_H
#define OUR_DESCRIPTOR_H

#include <stdint.h>

// Report IDs used in the combined HID report map. The host addresses each of our
// reports by these IDs; they also tag the report-reference descriptors NimBLE
// builds, which is how a host maps a notification back to mouse vs keyboard.
#define REPORT_ID_MOUSE     1
#define REPORT_ID_KEYBOARD  2

// A second, *relative* pointer, in its own HID service instance. It walks the
// cursor across a display boundary on a host we position absolutely (absolute X/Y
// is clamped to the display the cursor is on), and it carries every pointer report
// for a host that cannot be positioned absolutely at all — Windows addresses only
// its primary monitor that way. See our_descriptor.c for the shape's rationale.
#define REPORT_ID_MOUSE_REL 3

// Payload sizes (excluding the report ID, which the GATT report characteristic
// carries out of band).
#define MOUSE_REPORT_SIZE     7   // 1 button byte + X(16) + Y(16) + wheel(8) + pan(8)
#define MOUSE_REL_REPORT_SIZE 7   // same shape, X/Y relative
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
