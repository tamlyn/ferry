// Virtual-HID drag experiment: which report-map shapes will macOS synthesise
// drag events for?
//
// For each descriptor variant this creates a virtual HID device (IOHIDUserDevice —
// the same mechanism bluetoothd uses to surface BLE HID devices, so it models the
// HOGP path far better than a USB test would), injects move / button-down /
// drag-motion / button-up reports, and reads the window server's per-event-type
// counters to see what was synthesised. leftMouseDragged incrementing while the
// button is held = macOS drags for that shape.
//
// DEAD END on stock macOS, kept for reference + its still-working `watch` mode:
// creating an IOHIDUserDevice requires the com.apple.hid.manager.user-access-device
// or com.apple.developer.hid.virtual.device entitlement (IOHIDResourceUserClient
// checks nothing else — root does NOT help), and AMFI SIGKILLs ad-hoc-signed
// binaries claiming either. The drag question was settled with real firmware
// A/B builds instead. `swift tools/hidsim.swift watch` (no sudo, no entitlement)
// still prints window-server event-type counter deltas while you drag by hand.
//
// Run with:  sudo swift tools/hidsim.swift          (root needed to create the device)
// Keep hands OFF the real mouse while it runs — real input pollutes the counters.
// The injected drag sweeps the middle of whichever display the cursor is on, so
// park it somewhere a short left-drag can't do damage (e.g. over the desktop).

import Foundation
import IOKit.hid
import CoreGraphics

// ---- descriptor fragments (mirroring main/our_descriptor.c) -----------------

let MOUSE_ABS: [UInt8] = [
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x02,        // Usage (Mouse)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        //   Report ID (1)
    0x09, 0x01,        //   Usage (Pointer)
    0xA1, 0x00,        //   Collection (Physical)
    0x05, 0x09,        //     Usage Page (Button)
    0x19, 0x01, 0x29, 0x08,
    0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08,
    0x81, 0x02,        //     8 buttons
    0x05, 0x01,        //     Usage Page (Generic Desktop)
    0x09, 0x30, 0x09, 0x31,
    0x15, 0x00, 0x26, 0xFF, 0x7F,
    0x75, 0x10, 0x95, 0x02,
    0x81, 0x02,        //     absolute X/Y, 0..32767
    0x09, 0x38,
    0x15, 0x81, 0x25, 0x7F,
    0x75, 0x08, 0x95, 0x01,
    0x81, 0x06,        //     relative wheel
    0xC0, 0xC0,
]

let KEYBOARD: [UInt8] = [
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x06,        // Usage (Keyboard)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x02,        //   Report ID (2)
    0x05, 0x07,
    0x19, 0xE0, 0x29, 0xE7,
    0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08,
    0x81, 0x02,        //   modifiers
    0x75, 0x08, 0x95, 0x01,
    0x81, 0x03,        //   reserved
    0x05, 0x08,
    0x19, 0x01, 0x29, 0x05,
    0x75, 0x01, 0x95, 0x05,
    0x91, 0x02,        //   LED output
    0x75, 0x03, 0x95, 0x01,
    0x91, 0x03,        //   LED padding
    0x05, 0x07,
    0x19, 0x00, 0x29, 0xFF,
    0x15, 0x00, 0x26, 0xFF, 0x00,
    0x75, 0x08, 0x95, 0x06,
    0x81, 0x00,        //   6 keycodes
    0xC0,
]

let MOUSE_REL: [UInt8] = [   // button-less, as currently flashed
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x02,        // Usage (Mouse)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x03,        //   Report ID (3)
    0x09, 0x01,        //   Usage (Pointer)
    0xA1, 0x00,        //   Collection (Physical)
    0x09, 0x30, 0x09, 0x31,
    0x15, 0x81, 0x25, 0x7F,
    0x75, 0x08, 0x95, 0x02,
    0x81, 0x06,        //     relative X/Y
    0xC0, 0xC0,
]

let variants: [(name: String, desc: [UInt8])] = [
    ("mouse+kbd+rel (current firmware)", MOUSE_ABS + KEYBOARD + MOUSE_REL),
    ("mouse+kbd (no relative pointer)",  MOUSE_ABS + KEYBOARD),
    ("mouse only",                       MOUSE_ABS),
]

// ---- window-server event counters -------------------------------------------

let watched: [(String, CGEventType)] = [
    ("down",    .leftMouseDown),
    ("dragged", .leftMouseDragged),
    ("moved",   .mouseMoved),
    ("up",      .leftMouseUp),
]

func counters() -> [String: UInt32] {
    var out: [String: UInt32] = [:]
    for (name, type) in watched {
        out[name] = CGEventSource.counterForEventType(.combinedSessionState, eventType: type)
    }
    return out
}

func delta(_ a: [String: UInt32], _ b: [String: UInt32]) -> String {
    watched.map { "\($0.0)=\(b[$0.0]! &- a[$0.0]!)" }.joined(separator: " ")
}

// ---- report injection --------------------------------------------------------

func absReport(x: UInt16, y: UInt16, buttons: UInt8) -> [UInt8] {
    [0x01, buttons,
     UInt8(x & 0xFF), UInt8(x >> 8),
     UInt8(y & 0xFF), UInt8(y >> 8),
     0x00]
}

func send(_ dev: IOHIDUserDevice, _ report: [UInt8]) {
    let rc = report.withUnsafeBufferPointer {
        IOHIDUserDeviceHandleReportWithTimeStamp(dev, mach_absolute_time(),
                                                 $0.baseAddress!, $0.count)
    }
    if rc != kIOReturnSuccess {
        print("  !! HandleReport failed: 0x\(String(rc, radix: 16))")
    }
}

func runVariant(name: String, desc: [UInt8]) {
    print("\n=== \(name)")
    let props: [String: Any] = [
        kIOHIDReportDescriptorKey: Data(desc),
        kIOHIDVendorIDKey: 0x16C0,
        kIOHIDProductIDKey: 0x05DF,
        kIOHIDProductKey: "FerrySim",
        kIOHIDTransportKey: "Bluetooth Low Energy",
    ]
    guard let dev = IOHIDUserDeviceCreateWithProperties(kCFAllocatorDefault,
                                                        props as CFDictionary, 0) else {
        print("  !! could not create virtual HID device (run with sudo?)")
        return
    }
    Thread.sleep(forTimeInterval: 2.0)   // let the event services attach

    let midY: UInt16 = UInt16(0.55 * 32767)
    func sweep(from: Double, to: Double, buttons: UInt8) {
        for i in 0...30 {
            let x = UInt16((from + (to - from) * Double(i) / 30.0) * 32767)
            send(dev, absReport(x: x, y: midY, buttons: buttons))
            usleep(10_000)
        }
    }

    let c0 = counters()
    sweep(from: 0.35, to: 0.50, buttons: 0)          // plain motion
    Thread.sleep(forTimeInterval: 0.3)
    let c1 = counters()
    print("  plain motion:      \(delta(c0, c1))")

    send(dev, absReport(x: UInt16(0.50 * 32767), y: midY, buttons: 1))   // button down
    Thread.sleep(forTimeInterval: 0.15)
    let c2 = counters()
    sweep(from: 0.50, to: 0.65, buttons: 1)          // motion with button held
    Thread.sleep(forTimeInterval: 0.15)
    let c3 = counters()
    send(dev, absReport(x: UInt16(0.65 * 32767), y: midY, buttons: 0))   // button up
    Thread.sleep(forTimeInterval: 0.3)
    let c4 = counters()

    print("  button down:       \(delta(c1, c2))")
    print("  motion while held: \(delta(c2, c3))   <-- dragged>0 means macOS drags this shape")
    print("  button up:         \(delta(c3, c4))")

    if c1["moved"]! &- c0["moved"]! == 0 {
        print("  !! no mouseMoved during plain motion — device not driving the cursor;")
        print("     result not meaningful (enumeration or permission problem)")
    }
    // dev released by ARC; give the HID stack a moment to tear it down
    Thread.sleep(forTimeInterval: 1.0)
}

// ---- entry -------------------------------------------------------------------

if CommandLine.arguments.contains("smoke") {
    // Creation-only test: no reports injected, no cursor movement. Verifies the
    // entitlement/signing situation without touching the user's session.
    let props: [String: Any] = [
        kIOHIDReportDescriptorKey: Data(MOUSE_ABS),
        kIOHIDProductKey: "FerrySim-smoke",
    ]
    if IOHIDUserDeviceCreateWithProperties(kCFAllocatorDefault,
                                           props as CFDictionary, 0) != nil {
        print("smoke: virtual HID device created OK")
    } else {
        print("smoke: creation FAILED (entitlement not honoured)")
    }
    exit(0)
}

if CommandLine.arguments.contains("watch") {
    // No virtual device, no sudo: sample the counters once a second while the
    // user attempts a drag with the real device, to see what the window server
    // synthesises for it.
    print("watch mode: attempt a click-and-drag with the real mouse now (12 s)…")
    var prev = counters()
    for _ in 0..<12 {
        Thread.sleep(forTimeInterval: 1.0)
        let now = counters()
        print("  \(delta(prev, now))")
        prev = now
    }
} else {
    print("hidsim: virtual-HID drag experiment — keep hands off the real mouse")
    for v in variants {
        runVariant(name: v.name, desc: v.desc)
    }
}
print("\ndone")
