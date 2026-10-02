# Ferry (ESP32-S3)

A wireless KVM that needs no software on the computers it controls. Plug a USB mouse
and keyboard into one small board, pair it with two computers over Bluetooth LE, and
use the one set of peripherals for both. To move from one computer to the other, push
the cursor off the edge of the screen.

The board is an ESP32-S3-WROOM-1 N16R8 ([dev board](https://www.aliexpress.com/item/1005006418608267.html)).
Unlike most Bluetooth microcontrollers, the S3 has a native USB host controller, so it
can read a wired mouse and keyboard directly.

![Ferry hardware: an ESP32-S3 dev board with USB power and USB host ports, connected
to a USB hub carrying a keyboard and a mouse](Ferry.jpg)

## What it does

- Hosts a USB mouse and keyboard, plugged in directly or through a hub.
- Appears to each computer as an ordinary Bluetooth LE mouse and keyboard. You pair it
  from the computer's own Bluetooth settings; there are no drivers or agents to install.
- Stays connected to both computers at once and sends input to whichever one has
  control.
- Moves the cursor across every screen on the desk. Push the pointer off an edge and it
  lands on whatever display borders that edge, whether that's the other computer or
  another monitor on the same one. Nothing disconnects or reconnects, and there's no
  button to press.
- Turns the MX Master's gesture button into Mission Control on the Mac or Task View on
  Windows, since neither does anything with that button by default.

It's meant for a personal laptop and a work laptop sharing a desk: one good keyboard
and mouse drive both as if they were a single machine, and neither needs anything
installed.

## How it works

A normal mouse reports relative motion ("moved 3 left, 2 up") and the computer decides
where the cursor ends up, so the mouse never knows where the cursor is. Ferry keeps
its own virtual cursor instead. It adds up the mouse's motion on a model of the whole
desk, so it knows which display the cursor is on and when it has been pushed off an
edge.

The desk is one global coordinate space with every display of both computers in it as
a rectangle. Each rectangle belongs to one computer, so an edge between two of them is
either a seam between two monitors of the same computer or a boundary between the two
computers. Crossing a boundary just means sending input to the other computer.

The two operating systems have to be driven differently:

- The Mac gets absolute coordinates (0–32767 on each axis, like a graphics tablet), so
  Ferry can put its cursor exactly where it wants. macOS clamps an absolute position to
  the display its cursor is already on, though, so to reach the Mac's other monitor
  Ferry sends a short burst of relative motion across the seam and then goes back to
  absolute positioning.
- Windows maps absolute coordinates to its primary monitor only, which would leave its
  other displays unreachable. So the PC gets relative motion throughout. When the
  cursor arrives on the PC, Ferry sends one oversized movement that pins it into a
  known corner of the desktop, then moves it from there to the right spot. For that to
  line up, the PC's display sizes in the layout have to match its Windows desktop
  pixels exactly.

Ferry stays connected to both computers rather than switching between stored pairings,
because reconnecting to a bonded device takes about a second each way. That's far too
slow to feel like one desktop.

Input goes through five stages:

1. **USB host input** (`usb_input.c`). The mouse runs in HID report protocol: Ferry
   parses its report descriptor to find the buttons (including back, forward and
   extras), motion, and vertical and horizontal scroll. A mouse whose descriptor can't
   be parsed falls back to the basic boot report. The keyboard runs in boot protocol
   (modifiers plus up to six keys).
2. **Cursor model** (`cursor.c`). Accumulates relative motion into the virtual
   cursor's position in desk coordinates, with pointer acceleration, clamped to the
   current display. It flags a crossing only when the cursor is held against an edge
   for a moment, so a firm shove crosses but a fast flick that reaches the edge doesn't.
3. **Desk layout** (`layout.c`). Which display rectangles sit where and which computer
   owns each. For any edge and position it says what's on the other side, and whether
   that's the same computer or the other one.
4. **KVM routing** (`kvm.c`). Moves the cursor around the desk, sends input to the
   computer that owns the display it's on, and handles crossings: a relative nudge over
   a seam on the Mac, or a host switch at a boundary between computers.
5. **BLE HID peripheral** (`ble_hid.c`). Advertises, pairs, holds a connection to each
   computer and sends the reports. It uses the NimBLE host stack, which tracks
   notification subscriptions per connection, so two computers can subscribe to the
   same device at once.

The HID report layout (`our_descriptor.c`) is what the computers pair against, split
across two HID service instances:

| Report | Fields |
|--------|--------|
| Mouse (id 1) | 8 buttons; 16-bit absolute X and Y (0–32767); 8-bit vertical wheel; 8-bit horizontal pan |
| Keyboard (id 2) | modifiers; up to 6 keys; LED output (Caps/Num Lock) |
| Relative pointer (id 3, own HID service) | 8 buttons; 16-bit relative X and Y; 8-bit vertical wheel; 8-bit horizontal pan |

The relative pointer carries all of the PC's pointer input and the Mac's seam nudges.
Its axes are 16-bit so a single report can pin the PC's cursor into a corner. It has a
HID service to itself because macOS won't turn an absolute pointer's motion into a
drag if the same HID device also contains a relative pointer. That's the Bluetooth
equivalent of a separate USB interface, which is how DeskHop avoids the same problem.
The Mac's clicks always go on report 1: back when the relative pointer shared a device
with the absolute one and had buttons, macOS treated it as the click owner and ignored
report 1's buttons.

## Pairing and use

Pair from each computer's Bluetooth settings. The device advertises as "Ferry" and
pairs with Just Works (no PIN, since it has no keypad), giving an encrypted bond that
survives reboots. It keeps advertising while a connection slot is free, so the second
computer can find it too. Once both are connected, use the mouse and keyboard as normal
and push the cursor off a screen edge to switch.

The Mac is recognised by its Bluetooth identity address and always takes the first
host slot; the other computer takes the second. The order they connect in doesn't
matter.

The BOOT button on the board switches between the two desk layouts (see below). The
on-board LED flashes blue for layout A and green for layout B, and the choice is saved
across reboots.

## Current limitations

- **Hard-coded desk.** The displays, their sizes and which edges border which are
  compiled into `layout.c`. There are two layouts, both for one particular desk: a
  MacBook, a Windows laptop, and an external monitor plugged into either the Mac
  (layout A) or the PC (layout B). Ferry also expects one computer to be that Mac,
  identified by the address in `ble_hid.c`, and the other to be a Windows PC. Using it
  on another desk means editing both files.
- **Extras need a parseable mouse.** Back/forward, extra buttons and horizontal scroll
  depend on parsing the mouse's report descriptor. A mouse whose descriptor can't be
  parsed falls back to boot input: three buttons, motion and a vertical wheel.
- **Two computers.** The connection layer holds exactly two at once.

## Building

Firmware is built with ESP-IDF v6.0.2. See [AGENTS.md](AGENTS.md) for the build, flash
and log-capture workflow, the hardware constraints, and the Bluetooth implementation
notes.

## Related projects

[DeskHop](https://github.com/hrvach/deskhop) does the same job over wires. It's two
Raspberry Pi Picos joined through a digital isolator, each plugged into one computer
over USB, with the keyboard and mouse plugged into the device. The cursor moves
between computers at the screen edge and neither computer needs any software. I only
came across it after building Ferry. If you don't need wireless, it's the more mature
project, and the repo includes a PCB and a 3D-printable case.

## Credits

Ferry was inspired by [jfedor2's Screen Hopper](https://github.com/jfedor2/screen-hopper),
which first showed a hardware KVM that hands the cursor between machines without any
software on them. Ferry is a separate implementation on different hardware with a
different stack, but the idea came from there.
