#include "our_descriptor.h"

// Combined absolute-pointer mouse + boot-style keyboard report map.
//
// This is the product's external contract: the exact bytes a prior BLE proof of
// concept proved macOS *and* Windows accept over HOGP, honouring X/Y as an
// absolute position rather than a relative delta. Reproduce it as-is unless a
// host compatibility problem forces a change (see the HID interface note in
// AGENTS.md).
//
//   - mouse: 8 buttons (1 byte) + 16-bit absolute X/Y, logical 0..32767
//     (digitizer-style pointer) + a signed 8-bit relative scroll wheel + a
//     signed 8-bit horizontal pan (Consumer AC Pan).
//   - keyboard: standard 6-key boot report (modifiers + reserved + 6 keycodes),
//     plus the LED output report a host uses for Caps/Num Lock.
//   - relative pointer (report id 3, in a SEPARATE report map / HID service
//     instance): a second, motion-only mouse (relative X/Y, no buttons), used to
//     walk the cursor across a display boundary that absolute positioning cannot
//     cross (the host clamps absolute X/Y to the display the cursor is on). This
//     is what lets the cursor hop between two displays of one host. Changing
//     either map means a host descriptor-cache flush, not just a re-pair (see
//     gotcha #9).
const uint8_t our_report_descriptor[] = {
    // ---------------------------------------------------------------- Mouse
    0x05, 0x01,                    // Usage Page (Generic Desktop)
    0x09, 0x02,                    // Usage (Mouse)
    0xA1, 0x01,                    // Collection (Application)
    0x85, REPORT_ID_MOUSE,         //   Report ID (1)
    0x09, 0x01,                    //   Usage (Pointer)
    0xA1, 0x00,                    //   Collection (Physical)
    0x05, 0x09,                    //     Usage Page (Button)
    0x19, 0x01,                    //     Usage Minimum (Button 1)
    0x29, 0x08,                    //     Usage Maximum (Button 8)
    0x15, 0x00,                    //     Logical Minimum (0)
    0x25, 0x01,                    //     Logical Maximum (1)
    0x75, 0x01,                    //     Report Size (1)
    0x95, 0x08,                    //     Report Count (8)
    0x81, 0x02,                    //     Input (Data,Var,Abs)
    0x05, 0x01,                    //     Usage Page (Generic Desktop)
    0x09, 0x30,                    //     Usage (X)
    0x09, 0x31,                    //     Usage (Y)
    0x15, 0x00,                    //     Logical Minimum (0)
    0x26, 0xFF, 0x7F,              //     Logical Maximum (32767)
    0x75, 0x10,                    //     Report Size (16)
    0x95, 0x02,                    //     Report Count (2)
    0x81, 0x02,                    //     Input (Data,Var,Abs)
    0x09, 0x38,                    //     Usage (Wheel)
    0x15, 0x81,                    //     Logical Minimum (-127)
    0x25, 0x7F,                    //     Logical Maximum (127)
    0x75, 0x08,                    //     Report Size (8)
    0x95, 0x01,                    //     Report Count (1)
    0x81, 0x06,                    //     Input (Data,Var,Rel)
    0x05, 0x0C,                    //     Usage Page (Consumer)
    0x0A, 0x38, 0x02,              //     Usage (AC Pan)
    0x15, 0x81,                    //     Logical Minimum (-127)
    0x25, 0x7F,                    //     Logical Maximum (127)
    0x75, 0x08,                    //     Report Size (8)
    0x95, 0x01,                    //     Report Count (1)
    0x81, 0x06,                    //     Input (Data,Var,Rel)   horizontal pan
    0xC0,                          //   End Collection
    0xC0,                          // End Collection

    // ------------------------------------------------------------- Keyboard
    0x05, 0x01,                    // Usage Page (Generic Desktop)
    0x09, 0x06,                    // Usage (Keyboard)
    0xA1, 0x01,                    // Collection (Application)
    0x85, REPORT_ID_KEYBOARD,      //   Report ID (2)
    0x05, 0x07,                    //   Usage Page (Keyboard/Keypad)
    0x19, 0xE0,                    //   Usage Minimum (Left Control)
    0x29, 0xE7,                    //   Usage Maximum (Right GUI)
    0x15, 0x00,                    //   Logical Minimum (0)
    0x25, 0x01,                    //   Logical Maximum (1)
    0x75, 0x01,                    //   Report Size (1)
    0x95, 0x08,                    //   Report Count (8)
    0x81, 0x02,                    //   Input (Data,Var,Abs)   modifier byte
    0x75, 0x08,                    //   Report Size (8)
    0x95, 0x01,                    //   Report Count (1)
    0x81, 0x03,                    //   Input (Cnst,Var,Abs)   reserved byte
    0x05, 0x08,                    //   Usage Page (LEDs)
    0x19, 0x01,                    //   Usage Minimum (Num Lock)
    0x29, 0x05,                    //   Usage Maximum (Kana)
    0x75, 0x01,                    //   Report Size (1)
    0x95, 0x05,                    //   Report Count (5)
    0x91, 0x02,                    //   Output (Data,Var,Abs)  LED report
    0x75, 0x03,                    //   Report Size (3)
    0x95, 0x01,                    //   Report Count (1)
    0x91, 0x03,                    //   Output (Cnst,Var,Abs)  LED padding
    0x05, 0x07,                    //   Usage Page (Keyboard/Keypad)
    0x19, 0x00,                    //   Usage Minimum (0)
    0x29, 0xFF,                    //   Usage Maximum (255)
    0x15, 0x00,                    //   Logical Minimum (0)
    0x26, 0xFF, 0x00,              //   Logical Maximum (255)
    0x75, 0x08,                    //   Report Size (8)
    0x95, 0x06,                    //   Report Count (6)
    0x81, 0x00,                    //   Input (Data,Array)     6 keycodes
    0xC0,                          // End Collection

};

const uint16_t our_report_descriptor_length = sizeof(our_report_descriptor);

// ---------------------------------------------------- Relative pointer (id 3)
// A second mouse whose X/Y are *relative*. It has two jobs: nudging the cursor
// across a display seam on a host that we position absolutely (macOS clamps
// absolute X/Y to the display the cursor is on), and carrying *all* pointer input
// for a host we cannot position absolutely at all — Windows maps absolute X/Y to
// the primary monitor only, so its other displays are unreachable that way.
//
// That second job is why this is a full mouse rather than motion alone: if a
// host's motion arrived here while its clicks and scroll rode report 1, every
// click would teleport the cursor back to report 1's last absolute position.
//
// It lives in its own report map, exposed as a second HID service instance,
// because macOS never synthesises a drag from an absolute report's motion if the
// same HID device also contains a relative pointer — proven by A/B test:
// identical maps with and without this collection, drag only works without.
// DeskHop dodges the same trap on USB by putting its relative helper mouse on a
// separate interface; separate HOGP service instances are the BLE equivalent.
//
// The buttons are the known risk here. Back when this pointer *shared one device*
// with the absolute one, macOS elected it the click owner and ignored report 1's
// buttons, and mirroring clicks here recovered the click but not click-and-drag
// (macOS won't fuse a button held on one pointer with motion arriving on the
// other). Separate instances may well have changed that, but nothing depends on
// it: the Mac is only ever sent zero buttons here, so its clicks stay on report 1.
//
// X/Y are 16-bit so a single report can carry a whole desktop's worth of motion.
// That is what makes re-synchronising a relative host cheap: one over-range delta
// pins its cursor into a corner, which the host clamps to a position we know.
const uint8_t our_rel_report_descriptor[] = {
    0x05, 0x01,                    // Usage Page (Generic Desktop)
    0x09, 0x02,                    // Usage (Mouse)
    0xA1, 0x01,                    // Collection (Application)
    0x85, REPORT_ID_MOUSE_REL,     //   Report ID (3)
    0x09, 0x01,                    //   Usage (Pointer)
    0xA1, 0x00,                    //   Collection (Physical)
    0x05, 0x09,                    //     Usage Page (Button)
    0x19, 0x01,                    //     Usage Minimum (Button 1)
    0x29, 0x08,                    //     Usage Maximum (Button 8)
    0x15, 0x00,                    //     Logical Minimum (0)
    0x25, 0x01,                    //     Logical Maximum (1)
    0x75, 0x01,                    //     Report Size (1)
    0x95, 0x08,                    //     Report Count (8)
    0x81, 0x02,                    //     Input (Data,Var,Abs)
    0x05, 0x01,                    //     Usage Page (Generic Desktop)
    0x09, 0x30,                    //     Usage (X)
    0x09, 0x31,                    //     Usage (Y)
    0x16, 0x01, 0x80,              //     Logical Minimum (-32767)
    0x26, 0xFF, 0x7F,              //     Logical Maximum (32767)
    0x75, 0x10,                    //     Report Size (16)
    0x95, 0x02,                    //     Report Count (2)
    0x81, 0x06,                    //     Input (Data,Var,Rel)   relative X/Y
    0x09, 0x38,                    //     Usage (Wheel)
    0x15, 0x81,                    //     Logical Minimum (-127)
    0x25, 0x7F,                    //     Logical Maximum (127)
    0x75, 0x08,                    //     Report Size (8)
    0x95, 0x01,                    //     Report Count (1)
    0x81, 0x06,                    //     Input (Data,Var,Rel)
    0x05, 0x0C,                    //     Usage Page (Consumer)
    0x0A, 0x38, 0x02,              //     Usage (AC Pan)
    0x15, 0x81,                    //     Logical Minimum (-127)
    0x25, 0x7F,                    //     Logical Maximum (127)
    0x75, 0x08,                    //     Report Size (8)
    0x95, 0x01,                    //     Report Count (1)
    0x81, 0x06,                    //     Input (Data,Var,Rel)   horizontal pan
    0xC0,                          //   End Collection
    0xC0,                          // End Collection
};

const uint16_t our_rel_report_descriptor_length = sizeof(our_rel_report_descriptor);
