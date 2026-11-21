# Compatibility matrix / issue checklist

One row per physical board. This doubles as the issue tracker for hardware
findings during the port: any new hardware issue gets a row (or a note in the
board's `reference-boards/` file) before it is worked around in firmware.

| Board (file) | Chip marking | Flash | PSRAM | USB-C route | External PHY? | eFuse `USB_PHY_SEL` | LED | BOOT | Flash/monitor OK | Matrix fixture OK | Issues |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| [C3 #1](fixtures/reference-boards/esp32c3-supermini-generic-01.md) | `FN4P` (as reported; transcribe at intake); rev v0.4 per esptool 2026-09-26 | 4 MB XMC (confirmed 2026-09-26) | none | native USB-Serial/JTAG (confirmed 2026-09-26) | no | n/a (C3) | — | — | OK 2026-09-26 (unit 01) | — | — |
| [C3 #2](fixtures/reference-boards/esp32c3-supermini-generic-02.md) | `FN4P` (as reported; transcribe at intake) | — | — | — | — | n/a (C3) | — | — | not yet tested | — | — |
| [S3 #1](fixtures/reference-boards/esp32s3-supermini-generic-01.md) | `FH4R2P` (as reported; transcribe at intake); rev v0.2 per esptool 2026-09-26 | 4 MB XMC (confirmed 2026-09-26) | 2 MB AP_3v3 (confirmed 2026-09-26) | native USB-Serial/JTAG (confirmed 2026-09-26) | no | dump pending | — | — | OK 2026-09-26 (unit 01) | — | — |
| [S3 #2](fixtures/reference-boards/esp32s3-supermini-generic-02.md) | `FH4R2P` (as reported; transcribe at intake) | — | — | — | — | — | — | — | not yet tested | — | — |
| [Tab 1a proto #1](tab1a-hardware-map.md) | JC3248W535EN-S3 (SoM marking to transcribe at next bench session) | 16 MB | 8 MB octal | native USB-Serial/JTAG (19/20) | no | n/a (native PHY in use) | panel BL IO1 | breakout | display+touch OK 2026-11-21 (b05, [findings §16](axs15231b-bringup-findings.md)); BLE-on flash pending | n/a (no matrix yet; devkit keys pending h01 bench) | [c02] BLE assert + storage 261; [h01] R1 P3 third signal open |

## Conventions

- `eFuse USB_PHY_SEL`: `unburned` / `burned` / `n/a (C3)`. Read-only checks
  only; burning is a Phase 8 sacrificial-board procedure requiring explicit
  approval.
- `External PHY?`: `yes` / `no` / `unknown` — determines the Phase 8 USB-PHY
  gate outcome.
- A board only counts as a **qualified reference** when every column is
  filled and Flash/monitor + Matrix fixture are both OK.
