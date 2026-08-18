#!/usr/bin/env python3
"""Long-running passive capture of Ferry's UART0 console.

Holds the port open indefinitely and reopens it whenever the cable comes and goes
(the board is on the monitor hub, so it disappears with the laptop), appending to a
per-day log plus a stable <root>-events.log holding only the lines worth reacting
to -- reboots, wedges, self-healed stalls, faults, and its own connection gaps -- so
a freeze noticed at 14:32 can be found without reading the whole capture.

Needs pyserial (bundled with esptool, on PATH once the ESP-IDF env is activated).

Usage:  python tools/capture.py [root]   (default root "ferry"; Ctrl-C to stop)

It never pulses the reset lines. The ESP32-S3's EN pin hangs off RTS and GPIO0 --
the layout selector -- off DTR, so a careless open reboots the board or fakes a
layout switch. pyserial deasserts both, but only *after* os.open() has already let
the tty driver assert them, and that window is long enough for the EN line's RC to
fire (seen: a POWERON reset 200 ms after attaching). So we clear the modem bits on
the raw fd the instant it exists, before pyserial gets to configure anything.
"""
import datetime, fcntl, os, re, struct, sys, time
import serial
import serial.serialposix as sp

# The dev-kit's UART-bridge port (WCH CH9102) -- the same port used to flash, never
# the native USB port, which hosts the mouse + keyboard. Override with FERRY_PORT.
PORT = os.environ.get("FERRY_PORT", "/dev/cu.usbmodem5C843191521")
NOTABLE = re.compile(rb"previous boot ended|wedged|recovered after|<-- fault|rst:0x|# port ")

_MODEM_CLEAR = struct.pack("I", sp.TIOCM_DTR | sp.TIOCM_RTS)
_os_open = os.open


def _quiet_open(path, flags, *args, **kw):
    fd = _os_open(path, flags, *args, **kw)
    try:
        fcntl.ioctl(fd, sp.TIOCMBIC, _MODEM_CLEAR)
    except OSError:
        pass
    return fd


os.open = _quiet_open

root = sys.argv[1] if len(sys.argv) > 1 else "ferry"
events = open(root + "-events.log", "ab", buffering=0)


def stamp():
    return datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]


def emit(f, line):
    f.write(line + b"\n")
    if NOTABLE.search(line):
        events.write(line + b"\n")


def open_port():
    s = serial.Serial()
    s.port = PORT
    s.baudrate = 115200
    s.timeout = 0.2
    s.dtr = False
    s.rts = False
    s.open()
    return s


port = None
buf = b""
day = None
f = None
missing_since = None

while True:
    today = datetime.date.today()
    if today != day:
        if f:
            f.close()
        f = open("%s-%s.log" % (root, today.strftime("%m%d")), "ab", buffering=0)
        day = today

    if port is None:
        if not os.path.exists(PORT):
            if missing_since is None:
                missing_since = time.time()
                emit(f, ("%s # port gone (cable unplugged?) - waiting" % stamp()).encode())
            time.sleep(2)
            continue
        try:
            port = open_port()
        except Exception as exc:
            time.sleep(2)
            continue
        gap = "" if missing_since is None else " after %ds away" % (time.time() - missing_since)
        missing_since = None
        buf = b""
        emit(f, ("%s # port opened%s" % (stamp(), gap)).encode())

    try:
        data = port.read(4096)
    except Exception as exc:
        emit(f, ("%s # port lost: %s" % (stamp(), exc)).encode())
        try:
            port.close()
        except Exception:
            pass
        port = None
        continue

    if not data:
        continue
    buf += data
    while b"\n" in buf:
        line, buf = buf.split(b"\n", 1)
        emit(f, stamp().encode() + b" " + line.rstrip(b"\r"))
