# Ferry — developer notes

ESP32-S3 firmware for a no-host-software wireless KVM: USB mouse + keyboard in, BLE
HID out to **two computers at once**, cursor hops between them by absolute
positioning. See [README.md](README.md) for what it does and the architecture; this
file is the build workflow, the hardware constraints, and the Bluetooth gotchas we
hit so they don't have to be rediscovered.

## Build & flash

ESP-IDF **v5.5.4**, installed via `eim` (Espressif's installer). The environment
must be activated before any `idf.py` — source the script matching the shell that
actually runs the command:

```sh
# bash (the shell Claude's Bash tool runs)
source ~/.espressif/tools/activate_idf_v5.5.4.sh && idf.py build
```
```fish
# fish (Tamlyn's interactive shell)
source ~/.espressif/tools/activate_idf_v5.5.4.fish
```

- **Flash** over the UART-bridge port (see hardware constraints — *not* the native
  USB port): `idf.py -p /dev/cu.usbmodem5C843191521 flash`.
- **Config changes need a clean regen.** Editing `sdkconfig.defaults` does *not*
  update an existing `sdkconfig`; the defaults are only applied when `sdkconfig` is
  generated. Run `rm sdkconfig && idf.py build`.
- `idf.py` becomes a shell function after activation, so `timeout idf.py …` fails —
  call `python "$IDF_PATH/tools/idf.py"` if you need to wrap it.

## Reading logs

`idf.py monitor` needs a TTY and won't run from Claude's Bash tool
("requires standard input to be attached to TTY"). Read UART0 directly with pyserial
(bundled with esptool, on PATH after activation): open the port at 115200, pulse RTS
(EN) to reset the chip, then read for a fixed window. Logs are on **UART0** because
the native USB port hosts the input devices. To watch a *live* session without
dropping its connections, open the port holding RTS/DTR deasserted (no reset) instead
of pulsing. Don't capture with `stty -f <port> …` + `cat`: macOS resets the termios
settings between the two opens, so the capture records garbage at the wrong baud.
Keep the port held open by one process (pyserial) for the whole session.

**Hold DTR deasserted while reading** (`p.dtr = False`). The BOOT button (GPIO0) is
the layout selector (see below), and GPIO0 is tied into the serial auto-reset circuit
— asserting DTR, which pyserial does by default on open, pulls GPIO0 low and registers
as a phantom layout-switch press (seen: the device jumped Config A → B the moment a
monitor connected). Standalone operation is unaffected; only a serial host toggling
those lines does it.

## Clearing bonds (and the mandatory host-side step)

`idf.py -p <port> erase-flash` wipes the whole chip including the NVS partition (all
bonds), then `idf.py -p <port> flash` reinstalls. **After erasing the device you must
also "forget" it on every computer it was paired with** — otherwise each host keeps
auto-reconnecting with keys the device no longer has (see gotcha #4).

## Hardware constraints

The board is a 44-pin **ESP32-S3-WROOM-1 N16R8** dev board
([AliExpress listing](https://www.aliexpress.com/item/1005006418608267.html)):
16 MB flash, 8 MB PSRAM, dual USB-C (one native USB-OTG, one UART bridge via a WCH
**CH9102**, VID `0x1a86`, enumerating at `/dev/cu.usbmodem5C843191521`).

- **Two USB ports, two jobs.** On the S3 the native USB controller and the
  USB-Serial-JTAG console share the same D+/D- pins (GPIO19/20) — only one can be used
  at a time. The native port hosts the mouse + keyboard, so the console goes out over
  **UART0** (`CONFIG_ESP_CONSOLE_UART_DEFAULT`). Flash and read logs over the dev-kit's
  **UART-bridge** port; reserve native USB (OTG) for the input devices.
- **OTG VBUS solder jumper.** The dev board did not drive 5 V on the OTG port's VBUS
  by default, so hosted devices got no power and never enumerated (silent — no
  errors). The two "USB-OTG" pads on the back were bridged with solder to connect the
  5 V rail to VBUS. If USB hosting ever stops working (devices don't power up), check
  that joint first. Side effect: the OTG port now always sources 5 V, so don't plug
  the board into a PC as a USB *device* via that port (two 5 V sources would fight).
- **USB hub support needs ESP-IDF ≥ 5.5.** v5.4's experimental external-hub code
  asserted and reboot-looped on hub enumeration glitches; v5.5 makes low-speed
  devices behind a hub supported. That's why the project is pinned to v5.5.4.
- **Watch — standalone-power stability.** Seen once: a few seconds unresponsive, LED
  flash, then recovery — but it coincided with swapping the USB power source, i.e. a
  plain power-cycle (the boot log confirmed a clean `POWERON` reset, and it then ran
  50 s of heavy input on Mac-USB power with no reset). So *that* instance was benign.
  Keep an eye on it when running standalone off a wall adapter: a reboot *while sitting
  steady* on adapter power points at a brownout under the hub + two HID devices + BLE
  radio load — try a beefier supply or bulk capacitance on the 5 V rail before
  suspecting firmware.

## USB HID input

The mouse runs in **report protocol** (not boot): `usb_input.c` fetches its report
descriptor and a small parser (`mouse_fmt_parse`) locates the button, X/Y, wheel and
AC-Pan fields, so back/forward/extra buttons and horizontal scroll come through —
none of which the 3-button boot report carries. A mouse whose descriptor we can't
parse (no byte-aligned X/Y) falls back to the boot report. The **keyboard stays on
boot protocol** — all we need. Gotchas, mostly hit with a Logitech Bolt receiver
hosting an MX Master 3S:

- **Only boot mouse/keyboard interfaces are claimed.** The Bolt receiver enumerates
  **three** interfaces (boot keyboard, boot mouse, a vendor HID++ one); a directly
  attached keyboard adds more. Claiming the vendor/extra interfaces exhausts the
  S3's handful of USB host channels — `No more HCD channels available`, and then the
  *already-open* interfaces stop polling (silent). So we skip any interface that
  isn't `HID_PROTOCOL_MOUSE`/`_KEYBOARD`, exactly as the channel budget requires.

- **A multi-device (Easy-Switch) mouse only reports on the channel it's switched to.**
  If the MX Master is on its Bluetooth-to-a-computer channel, the Bolt receiver's
  mouse interface is **dead silent** — enumerates fine, delivers zero reports, looks
  like a firmware bug but isn't. For any capture/test, first tap the mouse's
  Easy-Switch to the channel paired with the dongle in the board (LED goes solid).

- **MX Master 3S map (report id 2, 9 bytes on the wire):** buttons byte — bit0 L,
  bit1 R, bit2 middle, bit3 **back**, bit4 **forward**, bit5 **gesture paddle**
  (button 6); then 16-bit rel X, 16-bit rel Y, 8-bit wheel, 8-bit AC Pan. The gesture
  paddle *is* forwarded (button 6) but macOS has no default action for it.

- **Adding AC Pan grew the mouse report 6→7 bytes**, which is a report-map change:
  extra *buttons* work without re-pairing (byte 0 is unchanged, the map already
  declared 8), but **horizontal scroll needs the host descriptor-cache flush** —
  forget device + toggle Bluetooth off/on + re-pair, per BLE gotcha #9.

## BLE stack

Host stack is **NimBLE**, not Bluedroid. NimBLE tracks CCCD (notification-enabled)
state **per connection**, which is what lets two hosts each subscribe independently;
Bluedroid keeps one shared CCCD value across connections and cannot. NimBLE also ships
built-in HOGP service builders (`ble_svc_hid` / `ble_svc_bas` / `ble_svc_dis`), so we
don't hand-build the GATT attribute table. `esp_hid`'s device layer is single-host on
*both* stacks ("there can be only one BLE HID device"), so the connection/send layer
in `ble_hid.c` is ours regardless.

The HID report maps in `our_descriptor.c` are the **external contract** the hosts
pair against, split across **two HID service instances**: instance one is the mouse
(report id 1: 8 buttons + absolute X/Y + a relative scroll wheel + a horizontal AC
Pan byte) + keyboard (id 2); instance two is a motion-only relative pointer (id 3,
relative X/Y, used to nudge the cursor across a same-host display seam — see `kvm.c`
/ `layout.c`). Two macOS behaviours force that shape, both established by A/B testing
on this device:

- **The relative pointer must live in a separate HID service.** If an absolute and
  a relative pointer share one HID device, macOS moves the cursor and clicks but
  never synthesises a drag from the absolute report's motion — the identical map
  minus the relative collection drags fine. DeskHop dodges the same trap on USB by
  putting its relative helper mouse on a separate interface; separate HOGP service
  instances are the BLE equivalent (NimBLE supports them natively;
  `CONFIG_BT_NIMBLE_SVC_HID_MAX_INSTANCES=2`).
- **Report 3 must stay button-less.** When it had buttons (while still sharing one
  device), macOS elected the *relative* pointer the click owner and ignored report
  1's buttons; mirroring clicks onto report 3 recovered the click but not
  click-and-drag, because macOS won't fuse a button held on one pointer with motion
  arriving on the other. Buttons ride report 1 only.

The mouse+keyboard map and the scroll wheel are proven on macOS + Windows.

## BLE gotchas (read before touching `ble_hid.c` or the BLE `sdkconfig`)

Every one of these cost real debugging time. They interact, and several only show up
on macOS or only with two hosts.

1. **macOS needs encryption-gated characteristics — `CONFIG_BT_NIMBLE_SM_LVL=2`.**
   HOGP requires the HID characteristics to demand encryption. macOS enforces it: it
   only *starts* pairing when it hits an "insufficient encryption" error accessing a
   characteristic. At the default level 0 the reports are readable in the clear, so
   macOS connects, discovers, subscribes — and never bonds, leaving its Bluetooth UI
   spinning forever. Windows tolerates level 0; macOS does not. Level 2 requires
   encryption *without* authentication, so the keypad-less Just Works pairing still
   satisfies it.

2. **Bond/CCCD store overflow silently unpairs a live host.** Each bonded host stores
   5 CCCDs (service-changed, battery, mouse report, keyboard report, relative-pointer
   report), so two hosts need 10 — already past the NimBLE default 8-CCCD store, and
   the default 3-bond limit leaves only one host of headroom. When the store
   overflows, `ble_store_util_status_rr` deletes the **oldest** peer — so one stray
   third pairing (e.g. re-pairing a host under a new identity address) evicts a real
   host's bond behind its back. That host keeps *its* keys, fails encryption on
   reconnect, and looks mysteriously dead until removed and re-paired. Fixed with
   `CONFIG_BT_NIMBLE_MAX_BONDS=4` and `CONFIG_BT_NIMBLE_MAX_CCCDS=16`.

3. **CONNECT arrives late — allocate per-connection state on first sight of a conn
   handle, not in the CONNECT handler.** This NimBLE fork defers the app-level
   `BLE_GAP_EVENT_CONNECT` behind a remote version/feature HCI exchange, so a bonded
   host's `ENC_CHANGE` and CCCD-restore `SUBSCRIBE` events land ~60 ms *before*
   CONNECT. `slot_find_or_alloc()` allocates a slot for any event naming a conn handle
   so restored subscriptions aren't dropped. Exception: a `SUBSCRIBE` with
   reason `TERM` fires during teardown — never allocate for it.

4. **Stale-bond reconnect loop after wiping the device.** If the device's bonds are
   erased but a host still has its bond, the host auto-reconnects with keys the device
   no longer has: `encryption change status=7` (`BLE_HS_ENOTCONN`), then disconnect
   `reason=531` (HCI 0x13, remote terminated), looping ~1.4×/sec. With two stale hosts
   the reconnect storm also crowds out any fresh pairing. Fix: **forget the device on
   every host**, one at a time — turn the *other* host's Bluetooth off so its storm
   doesn't interfere — then pair fresh. Success in the log looks like
   `status=0 encrypted=1 bonded=1` followed by `subscribe … mouse=1 kbd=1 rel=1`.

5. **Advertising while connected works.** Restart advertising in the CONNECT handler
   whenever a slot is still free, so a second host can find us — NimBLE stops
   advertising when a connection forms. (This works reliably here; no need for the
   deferred-restart-task pattern some multi-connection examples use.)

6. **Report value handles are captured by registration order** in
   `gatt_svr_register_cb` — the report characteristics all share UUID 0x2A4D, and
   register in the order the two HID service instances list them in `params.rpts[]`:
   0 = mouse input, 1 = keyboard input, 2 = keyboard LED output (instance one), then
   3 = relative-pointer input (instance two). The mouse and keyboard handles are
   asserted non-zero at host sync (the relative-pointer handle is logged, not
   asserted).

7. **NimBLE log spam.** At INFO level NimBLE logs one line per notification, flooding
   UART0 on every mouse move. Pinned to WARNING (`CONFIG_BT_NIMBLE_LOG_LEVEL_WARNING`);
   our own `ble_hid` / `kvm` INFO logs still show.

8. **Open — PC cursor stutter when both hosts are live** (smooth with the PC alone;
   the Mac is smooth regardless). Params are healthy — both links 15 ms / latency 0
   (macOS connects at 30 ms and renegotiates down; Windows asks for 15 directly) —
   so the suspect is the two centrals' connection-event anchors drifting into
   alignment, making the controller skip one link's events in patches (would recur
   intermittently on a tens-of-seconds/minutes cadence). Diagnostics are already in
   the firmware: conn params log on connect/update, and failed sends log a
   rate-limited `mouse notify failed`. If stutter recurs **with** drop warnings →
   coalesce motion reports device-side (latest absolute position wins; flush button
   changes immediately). **Without** warnings → request 7.5 ms on the PC link via
   `ble_gap_update_params` so it interleaves harmonically with the Mac's 15 ms.

9. **Changing the report map needs a host descriptor-cache flush, not just a re-pair.**
   macOS caches the HID report map keyed by the device's identity address, so "Forget
   This Device" + re-pair reconnects against the *cached* map — a newly added field
   (e.g. the scroll wheel's 6th byte) is silently ignored while everything in the old
   byte layout keeps working. Force a real re-read: forget the device, toggle the
   host's Bluetooth **off then on** (flushes `bluetoothd`), then re-pair; reboot the
   host if that isn't enough. Tell-tale symptom: the new field does nothing, but
   motion/buttons/keys are all fine. This is what made the scroll wheel look broken
   even though the device was already sending correct 6-byte reports.

   **Windows caches it too** (per bond, not just macOS): the scroll wheel did nothing
   on Windows until the device was removed and re-paired, after which it worked
   immediately. Same flush — Remove device, toggle the Bluetooth radio (or uninstall
   the stale HID entry in Device Manager with *show hidden devices*), then re-pair.

## Observing BLE from the dev Mac

The Mac running the toolchain is also one of the two KVM hosts, so both ends of a BLE
problem are observable locally:

- **Is the device advertising?** A short Swift CoreBluetooth scanner (`swift
  scan.swift`, needs Bluetooth TCC — run outside the sandbox) sees the "Ferry"
  advert within seconds. Ground truth when the firmware's "advertising" log is in
  doubt.
- **Is the Mac bonded/connected?** `system_profiler SPBluetoothDataType` lists Ferry
  (address `68:EE:8F:63:97:32`) under Connected / Not Connected.

## Source layout (`main/`)

| File | Role |
|------|------|
| `ferry.c` | app entry; wires USB input → KVM, starts the layout selector |
| `usb_input.c` | USB host + HID decode: mouse in *report* protocol (a small descriptor parser locates the button/X/Y/wheel/AC-pan fields — extra buttons + horizontal scroll), keyboard in boot protocol; unparseable mice fall back to the boot report. See "USB HID input". |
| `cursor.c` | the single active cursor in global desk points; speed-based pointer acceleration (slow = precise, fast = 1:1); sustained any-edge push detection |
| `layout.c` | the desk model: display rectangles in one global coordinate space, each owned by a host; edge adjacency + the abs/relative crossing geometry. Two selectable layouts — A (external on Mac) and B (external on PC) — sharing geometry and differing only in host ownership |
| `kvm.c` | routes input through the desk; on an edge push crosses the cursor to the neighbouring display — relative nudge within a host (slot 0 = Mac), host switch between computers (slot 1 = PC). `kvm_request_layout` swaps layouts safely (applied on the input task, re-homing the cursor) |
| `control.c` | physical layout selector: BOOT button (GPIO0) cycles A/B, on-board WS2812 RGB LED (GPIO48) flashes the active layout's colour (A = blue, B = green), choice persisted in NVS |
| `ble_hid.c` | NimBLE HOGP peripheral: advertising, bonding, 2-slot connection layer (each host pinned to a slot by BLE identity), per-host report senders (abs mouse, keyboard, relative pointer) |
| `our_descriptor.c` | HID report maps, two HID service instances (mouse id 1 — buttons + abs X/Y + wheel + AC pan — & keyboard id 2; relative pointer id 3 separate) — the external contract |
