# Screen Hopper (ESP32-S3)

A wireless KVM that needs no host software: one USB mouse + keyboard drives two
computers over Bluetooth LE, and the cursor "hops" between machines by placing it
at **absolute** on-screen coordinates. See [PRD.md](PRD.md) for the full brief and
the milestone plan.

This board (ESP32-S3) was chosen for its **native USB host** capability — the one
piece the prior Bluetooth proof-of-concept (a Pico 2 W, `../screen-hopper-bt`)
could not do.

## Status — M2: single-host BLE bridge

The firmware implements **M2**: it hosts a USB mouse + keyboard (directly or
through a hub), accumulates the mouse's relative motion into an **absolute**
virtual cursor, and re-transmits both to one paired computer as a **BLE HID
(HOGP) peripheral** — full pointer + keyboard control with **no host-side
software**. Verified placing the cursor at absolute screen coordinates on
**macOS and Windows**, and bonds persist across power cycles (paired hosts
reconnect automatically).

Pair from the host's built-in Bluetooth — the device advertises as **"Screen
Hopper"**, and pairing is "Just Works" (no PIN; it has no keypad). The USB
devices are read in the HID **boot** protocol (mouse: buttons + relative dx/dy;
keyboard: modifiers + up to 6 keycodes).

Source layout (`main/`):

- `our_descriptor.c` — the combined **absolute-pointer** mouse (report id 1: 8
  buttons + 16-bit absolute X/Y, 0…32767) + boot keyboard (report id 2) HID
  report map. This is the external contract the hosts pair against.
- `ble_hid.c` — the BLE HOGP peripheral (Bluedroid + `esp_hid`): advertising,
  bonding, and the mouse/keyboard report senders.
- `cursor.c` — the absolute cursor model: accumulate relative motion into the
  0…32767 space (with a sensitivity gain) and clamp to bounds.
- `usb_input.c` — the M1 USB host + HID decode, behind a callback API.
- `screenhopper.c` — wires USB input → cursor → BLE.

**Known limitations:**

- **No scroll wheel yet.** The mouse is driven in HID boot protocol, whose report
  has no wheel byte, and our BLE descriptor has no wheel field. Adding scroll
  needs report-protocol parsing of the mouse's own descriptor — the deferred work
  noted under "Boot protocol only" below.
- **One display only.** The single logical coordinate space maps to one display,
  so the cursor can't cross to a second physical monitor from the device alone
  (M4).
- **One host at a time.** Holding two hosts at once and hopping between them is M3.

All are described in the PRD.

The console/logs come out over **UART0** (the native USB port hosts the input
devices — see the wiring note below), e.g.:

```
I (…) screenhopper: Screen Hopper (ESP32-S3) — M2: USB → absolute cursor → BLE HID
I (…) ble_hid: HID stack started, advertising
I (…) ble_hid: host connected
I (…) ble_hid: bonded and encrypted — ready to send input
I (…) usb_input: mouse connected
I (…) usb_input: keyboard connected
```

## Toolchain

Built with **ESP-IDF v5.4** (installed here via [`eim`](https://github.com/espressif/idf-im-cli),
Espressif's installation manager: `eim install -t esp32s3 -i v5.4`). The M1
firmware is confirmed to build clean against it.

Activate the environment in each new shell before running `idf.py`:

```fish
# fish (this machine's shell)
source ~/.espressif/tools/activate_idf_v5.4.fish
```

```sh
# bash / zsh
source ~/.espressif/tools/activate_idf_v5.4.sh
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
