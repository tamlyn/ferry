#!/usr/bin/env python3
# Long-running passive UART0 capture for the freeze hunt (see main/diag.c).
#
# Holds RTS *and* DTR deasserted so it neither resets the chip nor pulls GPIO0 low
# (which the serial auto-reset circuit would register as a phantom layout-switch
# press — see CLAUDE.md "Reading logs"). Prefixes each line with a wall-clock
# timestamp and tees to a logfile, so a freeze you notice at (say) 14:32 can be
# found in the log. Leave it attached during normal use; after the next freeze,
# read the log for `last reset: …` and any `BLE send wedged` / falling-heap lines.
#
# Needs pyserial (bundled with esptool, on PATH once the ESP-IDF env is activated).
#
# Usage:  python tools/capture.py [logfile]   (default ferry-capture.log; Ctrl-C to stop)

import datetime
import os
import sys

import serial

# The dev-kit's UART-bridge port (WCH CH9102). Override with FERRY_PORT if it
# enumerates elsewhere. This is the same port used to flash — never the native USB
# port, which hosts the mouse + keyboard.
PORT = os.environ.get("FERRY_PORT", "/dev/cu.usbmodem5C843191521")

logpath = sys.argv[1] if len(sys.argv) > 1 else "ferry-capture.log"

p = serial.Serial()
p.port = PORT
p.baudrate = 115200
p.rts = False
p.dtr = False
p.timeout = 0.2
p.open()
# macOS can toggle these on open; re-assert deasserted.
p.rts = False
p.dtr = False


def stamp():
    return datetime.datetime.now().strftime("%H:%M:%S")


buf = b""
with open(logpath, "a", buffering=1) as f:
    banner = f"===== capture started {datetime.datetime.now().isoformat(timespec='seconds')} on {PORT} ====="
    print(banner)
    f.write(banner + "\n")
    try:
        while True:
            data = p.read(4096)
            if not data:
                continue
            buf += data
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode("utf-8", "replace").rstrip("\r")
                out = f"[{stamp()}] {text}"
                print(out)
                f.write(out + "\n")
    except KeyboardInterrupt:
        pass
    finally:
        p.close()
