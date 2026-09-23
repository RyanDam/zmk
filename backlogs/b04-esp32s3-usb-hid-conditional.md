# b04 — Phase 8: ESP32-S3 SuperMini USB HID (conditional, USB-PHY gate)

- **Category:** b (Build, deploy & config)
- **Severity:** Medium
- **Status:** TODO
- **Effort (est):** L

## Problem
The S3 SuperMini's sole USB-C connector routes to GPIO19/20, whose internal PHY defaults
to USB Serial/JTAG. Using the internal PHY for OTG requires permanently burning the
`USB_PHY_SEL` eFuse; otherwise OTG needs an external PHY. Wired USB HID is therefore a
conditional, potentially irreversible hardware configuration — not an assumed feature —
and must pass an explicit decision gate before any implementation.

## Evidence
- `report.md:15` — the internal-PHY eFuse requirement; the first S3 release must be BLE-only.
- `report.md:89-96` — the S3 USB-PHY decision tree (external PHY → test without fuses; internal PHY only → no eFuse burns during normal development; fused variant only with documented repeatable tests and project acceptance).
- `implement_plan.md:342-370` — Phase 8 preconditions, tasks, exclusions, and exit gate.

## Why it matters
- Wired HID is the only deferred S3 capability, and the only ESP milestone with an
  irreversible-hardware risk. Doing it without the gate risks bricking the normal
  USB Serial/JTAG flash/recovery path on user hardware.

## Conservative fix
- Preconditions: Phases 1–4 complete on Zephyr 4.4.x, ZMK modern USB migration (a02) complete, S3 BLE variant (b03) stable, SuperMini USB-C wiring inspected and documented, project decision on accepting the irreversible `USB_PHY_SEL` eFuse, and a UART0 (GPIO43/44) recovery procedure plus a sacrificial test board available.
- Inspect the SuperMini schematic and continuity-test USB-C D+/D- to GPIO19/20; determine whether an external PHY exists; record eFuse state before every test.
- If an external PHY path exists: add an S3-only USB configuration/variant enabling upstream `usb_otg`/`zephyr_udc0` and the migrated ZMK USB HID; prove it without changing eFuses.
- If only the internal PHY is available: stop before functional HID testing and obtain explicit approval for a separate fused experimental variant, documenting that `USB_PHY_SEL` is permanent and changes/disables normal USB Serial/JTAG behavior.
- On a sacrificial board only: apply the approved fuse procedure and validate ROM download recovery and UART0 recovery before attempting ZMK USB HID. Never make this a CI or default-user requirement.
- Add an explicitly named `usb_otg_fused` S3 configuration only if the fuse path proves repeatable; keep the normal BLE-only/USB-Serial-JTAG S3 configuration unchanged.
- Verify controller setup, VBUS/power requirements, descriptors, and endpoint allocation against S3 DWC2 capabilities.
- Test USB-only, BLE-only while cabled, endpoint switching, cable reconnect, reset while attached, host suspend/resume, and recovery after malformed USB firmware.
- Add S3 USB build coverage and a hardware smoke-test checklist; automate enumeration/report checks with a host-side test rig where possible.

## Out of scope (for now)
- Any C3 USB configuration (its USB Serial/JTAG peripheral is not an HID-capable OTG controller).
- Burning `USB_PHY_SEL` or any eFuse on a normal development/CI board.

## Acceptance / verification
- [ ] Either (a) USB HID is rejected for the common SuperMini (unacceptable permanent fuse) and S3 remains BLE-only, or (b) the fused variant enumerates as a stable HID keyboard on Linux, macOS, and Windows.
- [ ] Keyboard/consumer/mouse reports, suspend/resume, BLE/USB switching, and UART recovery all pass on the qualifying variant.
- [ ] USB support documented as S3-specific, revision-specific, and explicitly fuse-qualified in documentation and CI.
- [ ] No eFuse was burned on any non-sacrificial board.
