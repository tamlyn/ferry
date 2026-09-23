#!/usr/bin/env python3
"""Pull the HCI trace dumps out of a capture.py log and convert them to btsnoop.

The firmware prints the controller's HCI ring buffer between "hci dump begin" and
"hci dump end" lines (see the HCI trace section of ble_hid.c). capture.py stamps
every line with the Mac's clock, which ESP-IDF's converter cannot parse, so this
strips the stamps, writes each dump to its own file next to the log, and runs the
converter on it. Open the resulting .btsnoop.log in Wireshark.

Usage:  python tools/hci_extract.py ferry-0923.log
        (needs the ESP-IDF environment activated, for IDF_PATH)
"""
import os
import re
import subprocess
import sys

STAMP = re.compile(r"^\d\d:\d\d:\d\d\.\d\d\d ")
BEGIN = re.compile(r"hci dump begin \((.*)\)")
END = re.compile(r"hci dump end")


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    log = sys.argv[1]
    idf = os.environ.get("IDF_PATH")
    if not idf:
        sys.exit("IDF_PATH is not set: activate the ESP-IDF environment first")
    converter = os.path.join(idf, "tools", "bt", "bt_hci_to_btsnoop", "bt_hci_to_btsnoop.py")

    dumps = []
    current = None
    with open(log, encoding="utf-8", errors="replace") as f:
        for raw in f:
            stamp = raw[:12] if STAMP.match(raw) else ""
            line = STAMP.sub("", raw).rstrip("\n")
            m = BEGIN.search(line)
            if m:
                current = {"stamp": stamp, "why": m.group(1), "lines": []}
                continue
            if current is None:
                continue
            if END.search(line):
                dumps.append(current)
                current = None
                continue
            current["lines"].append(line)

    if not dumps:
        sys.exit("no complete dumps found in %s" % log)

    base = os.path.splitext(log)[0]
    for i, d in enumerate(dumps):
        tag = "%s-hci%d" % (os.path.basename(base), i)
        out = "%s-hci%d.txt" % (base, i)
        with open(out, "w") as f:
            f.write("\n".join(d["lines"]) + "\n")
        print("%s  %s  %d lines  -> %s" % (d["stamp"] or "--:--:--.---", d["why"], len(d["lines"]), out))
        # The converter writes ./parsed_logs/parsed_log_<tag>.btsnoop.log
        subprocess.run([sys.executable, converter, "-p", out, "-o", tag, "--has-ts"], check=False)


if __name__ == "__main__":
    main()
