# Compatibility matrix / issue checklist

One row per physical board. This doubles as the issue tracker for hardware
findings during the port: any new hardware issue gets a row (or a note in the
board's `reference-boards/` file) before it is worked around in firmware.

| Board (file) | Chip marking | Flash | PSRAM | USB-C route | External PHY? | eFuse `USB_PHY_SEL` | LED | BOOT | Flash/monitor OK | Matrix fixture OK | Issues |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| C3 #1 | pending | — | — | — | — | — | — | — | — | — | — |
| C3 #2 | pending | — | — | — | — | — | — | — | — | — | — |
| S3 #1 | pending | — | — | — | — | — | — | — | — | — | — |
| S3 #2 | pending | — | — | — | — | — | — | — | — | — | — |

## Conventions

- `eFuse USB_PHY_SEL`: `unburned` / `burned` / `n/a (C3)`. Read-only checks
  only; burning is a Phase 8 sacrificial-board procedure requiring explicit
  approval.
- `External PHY?`: `yes` / `no` / `unknown` — determines the Phase 8 USB-PHY
  gate outcome.
- A board only counts as a **qualified reference** when every column is
  filled and Flash/monitor + Matrix fixture are both OK.
