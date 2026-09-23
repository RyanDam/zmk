# ESP32-C3 / ESP32-S3 port — working records

Working records for the ZMK ESP32-C3/S3 SuperMini port (see
[`../implement_plan.md`](../implement_plan.md) for the phase plan and
[`../backlogs/`](../backlogs/) for tracked items).

This directory holds **records, fixtures, and results** — not user-facing
documentation. User-facing hardware-integration pages are added in Phase 10
under `docs/docs/hardware-integration/`.

## Layout

| Path | Contents | Phase |
| --- | --- | --- |
| `phase0-baseline.md` | Recorded software baseline (commits, west revisions, toolchain, patch series) | 0 |
| `baseline/` | Archived CI results and build sizes | 0 |
| `fixtures/matrix-fixture.md` | Direct-GPIO matrix keyboard fixture: BOM, pin maps, conflict notes | 0 |
| `fixtures/flash-monitor-runbook.md` | Flash/monitor/recovery runbook per chip | 0 |
| `fixtures/reference-boards/` | Per-board documentation (photos, markings, wiring) | 0 |
| `compat-matrix.md` | Compatibility matrix / issue checklist per physical board | 0 |
| `test-results/format.md` | Test-results format definition | 0 |
| `test-results/samples/` | Demonstrated sample entries | 0 |

## Phase status

| Phase | Backlog item | Status |
| --- | --- | --- |
| 0 — Baseline and hardware fixtures | `d01` | in progress (see `phase0-baseline.md`) |
| 1 — Zephyr 4.4.1 upgrade | `b01` | not started |
| 2 — kscan → matrix input | `a01` | not started |
| 3 — Legacy USB migration | `a02` | not started |
| 4 — Upgrade regression gate | `d02` | not started |
| 5 — C3 BLE board | `b02` | not started |
| 6 — S3 BLE board | `b03` | not started |
| 7 — BLE power/reliability | `d03` | not started |
| 8 — S3 USB HID (conditional) | `b04` | not started |
| 9 — Optional capabilities | `a03`, `a04`, `a05` | not started |
| 10 — Docs/CI/release policy | `f01` | not started |

## Host test matrix

Repeatable procedures for the host OS matrix (Phase 0 task 6). Hardware
availability is recorded per host; where a host is unavailable the procedure
and a named owner are recorded instead.

| Host OS | Hardware | Availability | Pairing / typing procedure | Owner |
| --- | --- | --- | --- | --- |
| Linux | CI runner + dev machine | available | `bluetoothctl`: `power on`, `agent on`, `default-agent`, `scan on`, `pair <mac>`, `trust <mac>`, `connect <mac>`; verify typing with `evtest`/`wev` or a text editor | TBD |
| macOS | TBD | pending | System Settings → Bluetooth → pair; verify with any text field; `log stream --predicate 'subsystem == "com.apple.bluetooth"'` for diagnostics | TBD |
| Windows | TBD | pending | Settings → Bluetooth & devices → add device; verify in Notepad; Event Viewer → Bluetooth support for diagnostics | TBD |
| Android | TBD | pending | Settings → Bluetooth → pair; verify in a text field | TBD |
| iOS | TBD | pending | Settings → Bluetooth → pair; verify in Notes | TBD |
