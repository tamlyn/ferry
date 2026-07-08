# Ferry (ESP32-S3)

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
- **Hops** the cursor across a desk of screens: push the pointer off a screen edge
  and it crosses to whatever borders that edge — the other computer, or another
  display of the *same* computer (e.g. a Mac's built-in and external monitors) — with
  no disconnect, no reconnect, no button to press, no stall.

The intended setup is a personal laptop and a work laptop on one desk, driven by one
good keyboard and mouse, with the cursor crossing between them as though they were a
single machine — without installing anything on either.

## How it works

Everything hinges on **absolute pointing**. A normal mouse reports *relative* motion
("moved 3 left, 2 up"), and the computer decides where the cursor ends up — so the
device can never know where the cursor actually is. Ferry instead maintains
its own virtual cursor position and reports it to the computer as an **absolute**
coordinate in a fixed logical space (0…32767 on each axis), digitizer-style. Because
the device now *knows* where the cursor is, it can tell when the cursor has been
pushed off the edge of one display and place it on the display that borders that edge
— which is what a "hop" is.

Holding both computers connected simultaneously (rather than switching between stored
pairings) is what makes the hand-off instant: switching bonded devices costs about a
second of reconnect lag each way, far too slow to feel like one continuous desktop.

The displays of both computers are modelled as rectangles in one global coordinate
space — a virtual **desk**. Each rectangle belongs to a host, so an edge between two
of them is either a seam *within* one computer (a Mac's built-in and external
monitors) or a boundary *between* the two computers. Crossing a boundary between
computers is just a matter of switching which host Ferry drives. Crossing a seam
within a computer is subtler: the host clamps an absolute position to the display its
cursor is currently on, so absolute positioning alone cannot move the cursor onto that
computer's *other* monitor. Ferry gets it across with a brief burst of **relative**
motion (a second, relative pointer — report id 3), after which absolute positioning
re-sticks on the new display.

Input flows through these stages:

1. **USB host input** (`usb_input.c`) — enumerates the mouse and keyboard in HID
   boot protocol and decodes their reports (buttons + relative motion + scroll wheel;
   modifiers + up to six keycodes).
2. **Absolute cursor model** (`cursor.c`) — accumulates the mouse's relative motion
   into the virtual cursor's position in the desk's global coordinates, clamped to the
   current display, and flags when the cursor has been pushed *sustainedly* past any
   edge (a firm shove, not a fast flick).
3. **Desk layout** (`layout.c`) — the map of which display rectangles sit where and
   which host each belongs to; it answers, for any edge and position, what borders it
   there and whether that neighbour is the same computer or the other one.
4. **KVM routing** (`kvm.c`) — keeps the one active cursor moving through the desk,
   routes input to the host owning the display it is on, and on an edge push crosses
   it to the neighbouring display — nudging across a same-host seam with relative
   motion, or switching hosts across a boundary between computers.
5. **BLE HID peripheral** (`ble_hid.c`) — advertises and pairs as a Bluetooth LE
   HID-over-GATT device, holds a connection to each computer, and delivers the
   reports. It is built on the **NimBLE** host stack, which tracks each connection's
   notification state independently — the property that lets two computers subscribe
   to the same device at once.

The HID report layout (`our_descriptor.c`) is the device's external contract. The base
mouse + keyboard map is proven on both macOS and Windows; the scroll wheel and the
relative pointer are verified on macOS but not yet re-tested on Windows:

| Report | Fields |
|--------|--------|
| Mouse (id 1) | 8 buttons; 16-bit **absolute** X and Y (0…32767); 8-bit relative scroll wheel |
| Keyboard (id 2) | modifier bitmask; up to 6 concurrent keycodes; LED output (Caps/Num Lock) |
| Relative pointer (id 3) | 8 buttons; 8-bit **relative** X and Y — nudges the cursor across a same-host display seam |

## Pairing and use

Pair from each computer's built-in Bluetooth: the device advertises as **"Ferry"**,
and pairing is "Just Works" (no PIN — the device has no keypad), producing
a bonded, encrypted link that persists across reboots. It keeps advertising while a
second connection slot is free, so both computers can pair. Once both are connected,
drive the mouse and keyboard normally and push the cursor off a screen edge to hop.

The first computer to connect takes the first host slot, the second takes the second.
How their displays are laid out on the virtual desk — which edges border which — is a
fixed, compiled-in arrangement (see limitations), not something detected or chosen at
pairing time.

## Current limitations

- **Hard-coded desk layout.** The arrangement of displays — how many each computer
  has, their sizes, and which edges border which — is compiled in, currently a single
  configuration ("Config A": the developer's Mac with its built-in and external
  monitors, plus the second computer at a fixed position with a placeholder
  resolution). It also assumes the Mac connects first. There is no auto-detection or
  user configuration yet; adapting it to another desk means editing `layout.c`.
- **Vertical scroll only.** The mouse wheel scrolls vertically; horizontal pan is not
  passed through. The wheel is read opportunistically from the boot report's 4th
  byte, which most (but not all) mice supply.
- **Two computers.** The connection layer holds exactly two hosts live at once.

## Building

Firmware is built with ESP-IDF v5.5.4. See [CLAUDE.md](CLAUDE.md) for the full build,
flash, and log-capture workflow, the hardware wiring constraints, and the Bluetooth
implementation notes.

## Credits

Ferry was inspired by [jfedor2's Screen Hopper](https://github.com/jfedor2/screen-hopper),
which first showed the idea of a host-software-free hardware KVM that hands the cursor
between machines. Ferry is an independent, from-scratch implementation — different
board, different stack, different code — but it owes that project the concept.
