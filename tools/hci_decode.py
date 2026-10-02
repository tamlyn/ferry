#!/usr/bin/env python3
"""One line per packet from a Ferry HCI dump, for reading without Wireshark.

Input is a dump file as written by tools/hci_extract.py (one packet per line:
index, C:/E:/H:/D: tag, eight timestamp bytes, then the packet). Decodes the
commands, events, and L2CAP/ATT/SMP traffic a HID peripheral actually sees; anything
else is printed as its opcode with the raw bytes.

Times are seconds relative to the last packet in the dump, so a dump taken on a
disconnect reads as a countdown to the radio going silent.

Usage:  python tools/hci_decode.py notes/logs/ferry-0928-hci1.txt [--last N]
"""
import sys

CMD = {
    0x0C03: "Reset", 0x0406: "Disconnect", 0x041D: "Read Remote Version",
    0x2006: "LE Set Adv Params", 0x2008: "LE Set Adv Data", 0x2009: "LE Set Scan Rsp Data",
    0x200A: "LE Set Adv Enable", 0x2013: "LE Connection Update", 0x2016: "LE Read Remote Features",
    0x2019: "LE Start Encryption", 0x201A: "LE LTK Request Reply", 0x201B: "LE LTK Request Neg Reply",
    0x2020: "LE Remote Conn Param Req Reply", 0x2021: "LE Remote Conn Param Req Neg Reply",
    0x2022: "LE Set Data Length", 0x2024: "LE Write Suggested Default Data Length",
    0x2030: "LE Read PHY", 0x2031: "LE Set Default PHY", 0x2032: "LE Set PHY",
    0x2027: "LE Add Device To Resolving List", 0x202D: "LE Set Address Resolution Enable",
    0x0C7C: "Write Authenticated Payload Timeout", 0x0C7B: "Read Authenticated Payload Timeout",
    0x2005: "LE Set Random Address", 0x1405: "Read RSSI",
}
EVT = {
    0x05: "Disconnection Complete", 0x08: "Encryption Change", 0x0C: "Read Remote Version Complete",
    0x0E: "Command Complete", 0x0F: "Command Status", 0x13: "Number Of Completed Packets",
    0x30: "Encryption Key Refresh Complete", 0x57: "Authenticated Payload Timeout Expired",
    0x10: "Hardware Error",
}
LE_SUB = {
    0x01: "Connection Complete", 0x03: "Connection Update Complete", 0x04: "Read Remote Features Complete",
    0x05: "LTK Request", 0x06: "Remote Conn Param Request", 0x07: "Data Length Change",
    0x0A: "Enhanced Connection Complete", 0x0C: "PHY Update Complete", 0x02: "Advertising Report",
}
ATT = {
    0x01: "Error Rsp", 0x02: "MTU Req", 0x03: "MTU Rsp", 0x04: "Find Info Req", 0x05: "Find Info Rsp",
    0x06: "Find By Type Value Req", 0x07: "Find By Type Value Rsp", 0x08: "Read By Type Req",
    0x09: "Read By Type Rsp", 0x0A: "Read Req", 0x0B: "Read Rsp", 0x0C: "Read Blob Req",
    0x0D: "Read Blob Rsp", 0x10: "Read By Group Type Req", 0x11: "Read By Group Type Rsp",
    0x12: "Write Req", 0x13: "Write Rsp", 0x52: "Write Cmd", 0x1B: "Notification", 0x1D: "Indication",
    0x1E: "Confirmation",
}
SMP = {0x01: "Pairing Req", 0x02: "Pairing Rsp", 0x03: "Pairing Confirm", 0x04: "Pairing Random",
       0x05: "Pairing Failed", 0x06: "Encryption Info", 0x07: "Central Identification",
       0x08: "Identity Info", 0x09: "Identity Address Info", 0x0A: "Signing Info",
       0x0B: "Security Request", 0x0C: "Pairing Public Key", 0x0D: "Pairing DHKey Check"}
SIG = {0x12: "Conn Param Update Req", 0x13: "Conn Param Update Rsp", 0x01: "Command Reject"}
REASON = {0x08: "connection timeout", 0x13: "remote user terminated", 0x16: "local host terminated",
          0x3D: "MIC failure", 0x3E: "connection failed to establish", 0x22: "LMP response timeout"}


def le16(b, i):
    return b[i] | (b[i + 1] << 8)


def hexs(b):
    return " ".join("%02x" % x for x in b)


def interval(itvl):
    return "%.2fms" % (itvl * 1.25)


def decode_cmd(b):
    op = le16(b, 0)
    name = CMD.get(op, "opcode 0x%04x" % op)
    p = b[3:]
    if op == 0x2022 and len(p) >= 6:
        return "%s handle=%d tx=%d octets/%d us" % (name, le16(p, 0), le16(p, 2), le16(p, 4))
    if op == 0x0406 and len(p) >= 3:
        return "%s handle=%d reason=0x%02x" % (name, le16(p, 0), p[2])
    if op == 0x2013 and len(p) >= 12:
        return "%s handle=%d itvl=%s..%s latency=%d timeout=%dms" % (
            name, le16(p, 0), interval(le16(p, 2)), interval(le16(p, 4)), le16(p, 6), le16(p, 8) * 10)
    if op == 0x2020 and len(p) >= 12:
        return "%s handle=%d itvl=%s..%s latency=%d timeout=%dms" % (
            name, le16(p, 0), interval(le16(p, 2)), interval(le16(p, 4)), le16(p, 6), le16(p, 8) * 10)
    if op == 0x2032 and len(p) >= 5:
        return "%s handle=%d tx_phys=0x%02x rx_phys=0x%02x" % (name, le16(p, 0), p[3], p[4])
    if op == 0x200A and len(p) >= 1:
        return "%s %s" % (name, "on" if p[0] else "off")
    if op in (0x2019, 0x201A, 0x2016, 0x041D, 0x0C7C, 0x0C7B, 0x1405) and len(p) >= 2:
        return "%s handle=%d" % (name, le16(p, 0))
    return "%s [%s]" % (name, hexs(p))


def decode_evt(b):
    code = b[0]
    p = b[2:]
    if code == 0x3E:
        sub = p[0]
        name = "LE " + LE_SUB.get(sub, "subevent 0x%02x" % sub)
        q = p[1:]
        if sub in (0x01, 0x0A) and len(q) >= 18:
            role = "central" if q[3] == 0 else "peripheral"
            addr = ":".join("%02x" % x for x in q[5:11][::-1])
            off = 11 if sub == 0x01 else 23
            return "%s status=%d handle=%d role=%s peer=%s itvl=%s latency=%d timeout=%dms" % (
                name, q[0], le16(q, 1), role, addr, interval(le16(q, off)), le16(q, off + 2), le16(q, off + 4) * 10)
        if sub == 0x03 and len(q) >= 9:
            return "%s status=%d handle=%d itvl=%s latency=%d timeout=%dms" % (
                name, q[0], le16(q, 1), interval(le16(q, 3)), le16(q, 5), le16(q, 7) * 10)
        if sub == 0x04 and len(q) >= 11:
            return "%s status=%d handle=%d features=%s" % (name, q[0], le16(q, 1), hexs(q[3:11]))
        if sub == 0x05 and len(q) >= 2:
            return "%s handle=%d" % (name, le16(q, 0))
        if sub == 0x06 and len(q) >= 10:
            return "%s handle=%d itvl=%s..%s latency=%d timeout=%dms" % (
                name, le16(q, 0), interval(le16(q, 2)), interval(le16(q, 4)), le16(q, 6), le16(q, 8) * 10)
        if sub == 0x07 and len(q) >= 10:
            return "%s handle=%d tx=%d/%dus rx=%d/%dus" % (
                name, le16(q, 0), le16(q, 2), le16(q, 4), le16(q, 6), le16(q, 8))
        if sub == 0x0C and len(q) >= 5:
            return "%s status=%d handle=%d tx_phy=%d rx_phy=%d" % (name, q[0], le16(q, 1), q[3], q[4])
        return "%s [%s]" % (name, hexs(q))
    name = EVT.get(code, "event 0x%02x" % code)
    if code == 0x05 and len(p) >= 4:
        return "%s status=%d handle=%d reason=0x%02x (%s)" % (
            name, p[0], le16(p, 1), p[3], REASON.get(p[3], "?"))
    if code == 0x08 and len(p) >= 4:
        return "%s status=%d handle=%d enabled=%d" % (name, p[0], le16(p, 1), p[3])
    if code == 0x0E and len(p) >= 4:
        op = le16(p, 1)
        return "%s %s status=%d" % (name, CMD.get(op, "opcode 0x%04x" % op), p[3])
    if code == 0x0F and len(p) >= 4:
        op = le16(p, 2)
        return "%s %s status=%d" % (name, CMD.get(op, "opcode 0x%04x" % op), p[0])
    if code == 0x13 and len(p) >= 5:
        return "%s handle=%d n=%d" % (name, le16(p, 1), le16(p, 3))
    if code == 0x0C and len(p) >= 8:
        return "%s handle=%d version=%d company=0x%04x subversion=0x%04x" % (
            name, le16(p, 1), p[3], le16(p, 4), le16(p, 6))
    if code in (0x30, 0x57) and len(p) >= 3:
        return "%s handle=%d" % (name, le16(p, 1))
    return "%s [%s]" % (name, hexs(p))


def decode_acl(b):
    hf = le16(b, 0)
    handle = hf & 0x0FFF
    pb = (hf >> 12) & 3
    if pb == 1:
        return "handle=%d (continuation) [%s]" % (handle, hexs(b[4:]))
    if len(b) < 8:
        return "handle=%d [%s]" % (handle, hexs(b[4:]))
    cid = le16(b, 6)
    p = b[8:]
    if cid == 4 and p:
        op = p[0]
        name = ATT.get(op, "ATT opcode 0x%02x" % op)
        if op in (0x1B, 0x0A, 0x12, 0x52) and len(p) >= 3:
            return "handle=%d ATT %s attr=%d [%s]" % (handle, name, le16(p, 1), hexs(p[3:]))
        if op == 0x01 and len(p) >= 5:
            return "handle=%d ATT Error Rsp req=0x%02x attr=%d code=0x%02x" % (handle, p[1], le16(p, 2), p[4])
        if op in (0x02, 0x03) and len(p) >= 3:
            return "handle=%d ATT %s mtu=%d" % (handle, name, le16(p, 1))
        if op in (0x08, 0x10) and len(p) >= 7:
            return "handle=%d ATT %s %d..%d type=%s" % (handle, name, le16(p, 1), le16(p, 3), hexs(p[5:][::-1]))
        return "handle=%d ATT %s [%s]" % (handle, name, hexs(p[1:]))
    if cid == 5 and p:
        name = SIG.get(p[0], "L2CAP sig 0x%02x" % p[0])
        if p[0] == 0x12 and len(p) >= 12:
            return "handle=%d %s itvl=%s..%s latency=%d timeout=%dms" % (
                handle, name, interval(le16(p, 4)), interval(le16(p, 6)), le16(p, 8), le16(p, 10) * 10)
        if p[0] == 0x13 and len(p) >= 6:
            return "handle=%d %s result=%d" % (handle, name, le16(p, 4))
        return "handle=%d %s [%s]" % (handle, name, hexs(p[1:]))
    if cid == 6 and p:
        return "handle=%d SMP %s [%s]" % (handle, SMP.get(p[0], "0x%02x" % p[0]), hexs(p[1:]))
    return "handle=%d cid=%d [%s]" % (handle, cid, hexs(p))


def main():
    args = sys.argv[1:]
    last = None
    if "--last" in args:
        i = args.index("--last")
        last = int(args[i + 1])
        del args[i:i + 2]
    if len(args) != 1:
        sys.exit(__doc__)
    pkts = []
    for line in open(args[0]):
        parts = line.split()
        if len(parts) < 10 or ":" not in parts[1]:
            continue   # a truncated first line from the ring buffer's overflow point
        tag = parts[1][0]
        try:
            raw = bytes(int(x, 16) for x in [parts[1][2:]] + parts[2:])
        except ValueError:
            continue
        ts = int.from_bytes(raw[:8], "little") / 1e6
        pkts.append((ts, tag, raw[8:]))
    if not pkts:
        sys.exit("no packets")
    end = pkts[-1][0]
    if last:
        pkts = pkts[-last:]
    for ts, tag, b in pkts:
        try:
            if tag == "C":
                text = "CMD  " + decode_cmd(b)
            elif tag == "E":
                text = "EVT  " + decode_evt(b)
            elif tag == "H":
                text = "TX   " + decode_acl(b)
            elif tag == "D":
                text = "RX   " + decode_acl(b)
            else:
                text = "%s    [%s]" % (tag, hexs(b))
        except IndexError:
            text = "%s    (short) [%s]" % (tag, hexs(b))
        print("%9.3f  %s" % (ts - end, text))


if __name__ == "__main__":
    main()
