# Screen Hopper (ESP32-S3)

A wireless KVM that needs no host software: one USB mouse + keyboard drives two
computers over Bluetooth LE, and the cursor "hops" between machines by placing it
at **absolute** on-screen coordinates. See [PRD.md](PRD.md) for the full brief and
the milestone plan.

This board (ESP32-S3) was chosen for its **native USB host** capability — the one
piece the prior Bluetooth proof-of-concept (a Pico 2 W, `../screen-hopper-bt`)
could not do.

## Status — M3: two hosts + edge-push hop

The firmware implements **M3**: it hosts a USB mouse + keyboard (directly or
through a hub), accumulates the mouse's relative motion into a **per-host
absolute** virtual cursor, and re-transmits both to **up to two paired computers
at once** as a **BLE HID (HOGP) peripheral** — a wireless KVM with **no host-side
software**. Control follows the cursor: push it off the left/right screen edge
and it **hops** to the adjacent machine, no disconnect/reconnect stall.

Both host links are held live simultaneously, so hand-off is seamless. This is
built on the **NimBLE** host stack, which keeps HOGP notification (CCCD) state
**per connection** — the property that lets two hosts each subscribe
independently. (The earlier M2 build used Bluedroid, whose single shared CCCD
value cannot support a second host; the switch to NimBLE is what makes M3
possible, and NimBLE's built-in HOGP service builder replaces the hand-built
attribute table.)

Pair from each host's built-in Bluetooth — the device advertises as **"Screen
Hopper"** (and keeps advertising while a connection slot is free), and pairing is
"Just Works" (no PIN; it has no keypad), bonded + encrypted, persisted in NVS.
The USB devices are read in the HID **boot** protocol (mouse: buttons + relative
dx/dy; keyboard: modifiers + up to 6 keycodes).

Source layout (`main/`):

- `our_descriptor.c` — the combined **absolute-pointer** mouse (report id 1: 8
  buttons + 16-bit absolute X/Y, 0…32767) + boot keyboard (report id 2) HID
  report map. This is the external contract the hosts pair against — unchanged
  from M2, so no host needs to re-learn the device.
- `ble_hid.c` — the BLE HOGP peripheral on **NimBLE**: advertising, Just-Works
  bonding, a two-slot connection layer (one `conn_handle` + subscription state
  per host), and per-host report senders (`ble_hid_send_mouse/keyboard(host, …)`).
  Uses NimBLE's built-in `ble_svc_hid`/`bas`/`dis` service builders.
- `kvm.c` — the stack-independent routing brain: the active host, a `cursor_t`
  per host, and the edge-push hop (slot 0 = left screen, slot 1 = right).
- `cursor.c` — the absolute cursor model: a caller-owned `cursor_t` (the KVM owns
  one per host); accumulate relative motion into the 0…32767 space (with a
  sensitivity gain), clamp to bounds, and report a **sustained** push past an
  edge so the KVM can hop.
- `usb_input.c` — the M1 USB host + HID decode, behind a callback API.
- `screenhopper.c` — wires USB input → KVM → BLE.

**Known limitations:**

- **No scroll wheel yet.** The mouse is driven in HID boot protocol, whose report
  has no wheel byte, and our BLE descriptor has no wheel field. Adding scroll
  needs report-protocol parsing of the mouse's own descriptor — the deferred work
  noted under "Boot protocol only" below.
- **One display per host.** The single logical coordinate space maps to one
  display, so within a host the cursor can't cross to a second physical monitor
  from the device alone (M4). Hopping is **between hosts**, not between a host's
  own monitors.
- **Fixed left/right host order.** Hosts are ordered by connection (slot 0 = left,
  slot 1 = right) with no wrap-around; arranging screen positions is M4.

All are described in the PRD.

The console/logs come out over **UART0** (the native USB port hosts the input
devices — see the wiring note below), e.g.:

```
I (…) screenhopper: Screen Hopper (ESP32-S3) — M3: USB → absolute cursor → BLE HID KVM
I (…) ble_hid: nimble host task started
I (…) ble_hid: report handles: mouse=31 kbd=35
I (…) ble_hid: advertising as "Screen Hopper" (2 slot(s) free)
I (…) ble_hid: host connected (conn=1) -> slot 0
I (…) ble_hid: subscribe (conn=1 attr=31 notify=1): mouse=1 kbd=0
I (…) ble_hid: host connected (conn=2) -> slot 1
I (…) kvm: hop 0 -> 1 (off right edge)
```

## Toolchain

Built with **ESP-IDF v5.5.4** (installed here via [`eim`](https://github.com/espressif/idf-im-cli),
Espressif's installation manager: `eim install -t esp32s3 -i v5.5.4`). v5.5 is
required for reliable low-speed-behind-a-hub USB hosting (v5.4 crashed on hub
enumeration glitches).

Activate the environment in each new shell before running `idf.py`:

```fish
# fish (this machine's shell)
source ~/.espressif/tools/activate_idf_v5.5.4.fish
```

```sh
# bash / zsh
source ~/.espressif/tools/activate_idf_v5.5.4.sh
```

The `espressif/usb_host_hid` USB HID class driver (resolved to 1.2.0) is pulled
automatically by the component manager on first build — see `main/idf_component.yml`.

## Build, flash, monitor

```sh
idf.py set-target esp32s3      # first time only; already done in this tree
idf.py build
idf.py -p <PORT> flash monitor
```

`<PORT>` is the dev-kit's **UART** port (see the wiring note below), e.g.
`/dev/cu.usbserial-*` on macOS.

## Hardware notes (read before wiring)

- **Two ports, two jobs.** On the ESP32-S3 the native USB controller and the
  USB-Serial-JTAG console **share the same D+/D- pins (GPIO19/20)** — you can only
  use one at a time. M1 dedicates the native USB port to *hosting* the mouse and
  keyboard, so the console/logs come out over **UART0** instead (`sdkconfig`
  already sets `CONFIG_ESP_CONSOLE_UART_DEFAULT`). On a dual-USB dev-kit, flash and
  monitor over the **UART bridge** port, and reserve the **native USB (OTG)** port
  for the input devices.
- **VBUS / 5 V.** A hosted USB device needs 5 V on the port's VBUS pin. Many S3
  dev-kits do not drive VBUS on the OTG port by default; you may need to supply 5 V
  to the device (a powered hub, or the board's 5 V rail) and share ground.
- **Boot protocol only, for now.** M1 accepts devices that expose a HID boot
  interface (nearly all standard mice/keyboards, including unifying/Bolt-style
  receivers). Reading a device's own report descriptor — needed for wheels, extra
  buttons, and NKRO — comes with a later milestone.
