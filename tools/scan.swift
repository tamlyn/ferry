// Is the device actually advertising? — ground truth from the dev Mac's own radio,
// for when the firmware's "advertising as …" log is in doubt or has scrolled away.
//
// Prints every peripheral advertising the HOGP service (0x1812) or calling itself
// "Ferry", with the flags that matter: `connectable=true` means a host could take
// the link, and since `advertise_if_slot_free()` only starts advertising when
// `free_slots() > 0`, seeing the advert at all also proves a slot is free.
//
// This deliberately only SCANS, never connects. A CoreBluetooth connection from an
// app can end up bonding, and a stray third bond evicts a real host's keys behind
// its back (CLAUDE.md BLE gotcha #2) — the exact failure you'd be trying to debug.
//
// CoreBluetooth hides BLE hardware addresses, so the `id=` shown is macOS's own
// per-host peripheral UUID, *not* 68:EE:8F:63:97:32. Use
// `system_profiler SPBluetoothDataType` to see the address and bond state.
//
// Needs Bluetooth TCC permission for the terminal that runs it, so run it outside
// any sandbox (macOS prompts on first use; a sandboxed run just reports state 5 and
// discovers nothing).
//
// Usage:  swift tools/scan.swift [seconds]     (default 12)

import Foundation
import CoreBluetooth

let duration = CommandLine.arguments.count > 1
    ? (Double(CommandLine.arguments[1]) ?? 12) : 12

final class Scanner: NSObject, CBCentralManagerDelegate {
    private var central: CBCentralManager!
    private var seen = Set<UUID>()
    private(set) var total = 0
    private(set) var matches = 0

    override init() {
        super.init()
        central = CBCentralManager(delegate: self, queue: nil)
    }

    func centralManagerDidUpdateState(_ c: CBCentralManager) {
        guard c.state == .poweredOn else {
            print("bluetooth unavailable (state \(c.state.rawValue), want \(CBManagerState.poweredOn.rawValue) = poweredOn)")
            return
        }
        // Duplicates off: one line per device is enough to answer "is it there?".
        c.scanForPeripherals(withServices: nil,
                             options: [CBCentralManagerScanOptionAllowDuplicatesKey: false])
        print("scanning for \(Int(duration))s…")
    }

    func centralManager(_ c: CBCentralManager, didDiscover p: CBPeripheral,
                        advertisementData d: [String: Any], rssi: NSNumber) {
        guard !seen.contains(p.identifier) else { return }
        seen.insert(p.identifier)
        total += 1

        // The name rides the scan response, so prefer the advertised name over the
        // cached p.name — a renamed device would otherwise show its old identity.
        let name = (d[CBAdvertisementDataLocalNameKey] as? String) ?? p.name ?? "(unnamed)"
        let uuids = (d[CBAdvertisementDataServiceUUIDsKey] as? [CBUUID]) ?? []
        let services = uuids.map { $0.uuidString }
        let isHID = services.contains { $0 == "1812" }
        guard isHID || name.localizedCaseInsensitiveContains("ferry") else { return }

        matches += 1
        let connectable = (d[CBAdvertisementDataIsConnectable] as? NSNumber)?.boolValue ?? false
        print(">>> \(name)  rssi=\(rssi)  services=[\(services.joined(separator: ","))]"
              + "  connectable=\(connectable)  id=\(p.identifier.uuidString)")
    }
}

let scanner = Scanner()
let deadline = Date().addingTimeInterval(duration)
while Date() < deadline {
    RunLoop.current.run(until: Date().addingTimeInterval(0.2))
}

if scanner.matches == 0 {
    print("no HID advertiser found (\(scanner.total) peripherals seen) — "
          + "device not advertising, out of range, or both slots taken")
} else {
    print("\(scanner.matches) HID advertiser(s) of \(scanner.total) peripherals seen")
}
