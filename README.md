# Screen Hopper (ESP32-S3)

A wireless KVM that needs no host software: one USB mouse + keyboard drives two
computers over Bluetooth LE, and the cursor "hops" between machines by placing it
at **absolute** on-screen coordinates. See [PRD.md](PRD.md) for the full brief and
the milestone plan.

This board (ESP32-S3) was chosen for its **native USB host** capability — the one
piece the prior Bluetooth proof-of-concept (a Pico 2 W, `../screen-hopper-bt`)
could not do.

## Status — M1: USB host read

The firmware currently implements **M1 only**: it acts as a USB host, enumerates a
mouse and/or keyboard, and logs the decoded input. No BLE, no absolute-cursor
model yet — M1 exists to prove the board can host real HID devices. Devices are
read in the HID **boot** protocol (mouse: buttons + 8-bit dx/dy; keyboard:
modifiers + up to 6 keycodes).

Expected serial output once a device is plugged in:

```
I (…) screenhopper: ready — plug in a USB mouse or keyboard
I (…) screenhopper: mouse connected
I (…) screenhopper: mouse   [L..] dx=  -3 dy=   1
I (…) screenhopper: keyboard connected
I (…) screenhopper: keyboard mod=0x02 keys="Hello"
```

Later milestones (M2 single-host BLE bridge with the absolute-pointer descriptor,
M3 two hosts + hop) are described in the PRD.

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
