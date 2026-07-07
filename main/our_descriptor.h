#ifndef OUR_DESCRIPTOR_H
#define OUR_DESCRIPTOR_H

#include <stdint.h>

// Report IDs used in the combined HID report map. The host addresses each of our
// two reports by these IDs; they also tag the report-reference descriptors NimBLE
// builds, which is how a host maps a notification back to mouse vs keyboard.
#define REPORT_ID_MOUSE     1
#define REPORT_ID_KEYBOARD  2

// Payload sizes (excluding the report ID, which the GATT report characteristic
// carries out of band).
#define MOUSE_REPORT_SIZE     5   // 1 button byte + X(16) + Y(16)
#define KEYBOARD_REPORT_SIZE  8   // modifiers + reserved + 6 keycodes

// The cursor lives in an absolute coordinate space of 0..ABS_AXIS_MAX on both
// axes. This is the whole point of the project: the host treats X/Y as an
// absolute position, not a relative delta, so we can place the cursor anywhere
// (and later "hop" it between machines).
#define ABS_AXIS_MAX  0x7FFF   // 32767

extern const uint8_t our_report_descriptor[];
extern const uint16_t our_report_descriptor_length;

#endif
