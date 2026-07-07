# Screen Hopper (ESP32-S3)

A wireless KVM that needs **no software on the computers it controls**. Plug a USB
mouse and keyboard into one small board, pair it with two computers over Bluetooth
LE, and drive both from that single set of peripherals — moving the cursor from one
machine to the other by pushing it off the edge of the screen.

The board is an **ESP32-S3-WROOM-1 N16R8** ([dev board](https://www.aliexpress.com/item/1005006418608267.html)),
chosen because the ESP32-S3 has a native USB host controller — the one capability a
plain Bluetooth microcontroller lacks, and the reason a wired mouse and keyboard can
be read at all.

## What it does

- Hosts a real USB mouse + keyboard, directly or through a hub.
- Presents itself to each computer as an ordinary **Bluetooth LE HID** mouse +
  keyboard. The computers need no drivers, agents, or configuration — they just see
  a Bluetooth input device and pair with it from their built-in Bluetooth settings.
- Holds **two computers connected at once** and sends input to whichever one
  currently has control.
- **Hops** the cursor between the two machines: push the pointer off the right edge
  of the left computer and it reappears on the right computer (and back off its left
  edge to return) — no disconnect, no reconnect, no button to press, no stall.

The intended setup is a personal laptop and a work laptop on one desk, driven by one
good keyboard and mouse, with the cursor crossing between them as though they were a
single machine — without installing anything on either.

## How it works

Everything hinges on **absolute pointing**. A normal mouse reports *relative* motion
("moved 3 left, 2 up"), and the computer decides where the cursor ends up — so the
device can never know where the cursor actually is. Screen Hopper instead maintains
its own virtual cursor position and reports it to the computer as an **absolute**
coordinate in a fixed logical space (0…32767 on each axis), digitizer-style. Because
the device now *knows* where the cursor is, it can tell when the cursor has been
pushed to the edge of one screen and place it at the matching edge of the other —
which is what a "hop" is.

Holding both computers connected simultaneously (rather than switching between stored
pairings) is what makes the hand-off instant: switching bonded devices costs about a
second of reconnect lag each way, far too slow to feel like one continuous desktop.

Input flows through four stages:

1. **USB host input** (`usb_input.c`) — enumerates the mouse and keyboard in HID
   boot protocol and decodes their reports (buttons + relative motion; modifiers + up
   to six keycodes).
2. **Absolute cursor model** (`cursor.c`) — accumulates the mouse's relative motion
   into the virtual cursor, clamped to bounds, and flags when the cursor has been
   pushed *sustainedly* past a left/right edge (a firm shove, not a fast flick).
3. **KVM routing** (`kvm.c`) — keeps one cursor per computer, sends input to the
   active one, and on an edge push hands control to the adjacent computer, entering
   its screen from the opposite edge at the same height.
4. **BLE HID peripheral** (`ble_hid.c`) — advertises and pairs as a Bluetooth LE
   HID-over-GATT device, holds a connection to each computer, and delivers the
   reports. It is built on the **NimBLE** host stack, which tracks each connection's
   notification state independently — the property that lets two computers subscribe
   to the same device at once.

The HID report layout (`our_descriptor.c`) is the device's external contract, proven
to be accepted by both macOS and Windows:

| Report | Fields |
|--------|--------|
| Mouse (id 1) | 8 buttons; 16-bit **absolute** X and Y (0…32767) |
| Keyboard (id 2) | modifier bitmask; up to 6 concurrent keycodes; LED output (Caps/Num Lock) |

## Pairing and use

Pair from each computer's built-in Bluetooth: the device advertises as **"Screen
Hopper"**, and pairing is "Just Works" (no PIN — the device has no keypad), producing
a bonded, encrypted link that persists across reboots. It keeps advertising while a
second connection slot is free, so both computers can pair. Once both are connected,
drive the mouse and keyboard normally and push the cursor off a screen edge to hop.

The first computer to connect is the **left** screen, the second is the **right**;
there is no wrap-around.

## Current limitations

- **No scroll wheel.** The mouse is read in HID boot protocol, which has no wheel
  byte, and the BLE report has no wheel field. Scroll needs report-protocol parsing
  of the mouse's own descriptor.
- **One display per computer.** The single logical coordinate space maps to one
  display, so within a computer the cursor can't cross onto a second physical
  monitor from the device alone. Hopping is *between computers*, not between a
  computer's own monitors.
- **Fixed left/right order.** Computers are ordered by connection, with no way yet to
  arrange their screen positions.

## Building

Firmware is built with ESP-IDF v5.5.4. See [CLAUDE.md](CLAUDE.md) for the full build,
flash, and log-capture workflow, the hardware wiring constraints, and the Bluetooth
implementation notes.
