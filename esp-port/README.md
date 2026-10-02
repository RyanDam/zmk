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
| 0 — Baseline and hardware fixtures | `d01` | done (see `phase0-baseline.md`) |
| 1 — Zephyr 4.4.1 upgrade | `b01` | done |
| 2 — kscan → matrix input | `a01` | done |
| 3 — Legacy USB migration | `a02` | done |
| 4 — Upgrade regression gate | `d02` | done |
| 5 — C3 BLE board | `b02` | in progress (branch `feat/esp32c3`) |
| 6 — S3 BLE board | `b03` | not started |
| 7 — BLE power/reliability | `d03` | not started |
| 8 — S3 USB HID (conditional) | `b04` | not started |
| 9 — Optional capabilities | `a03`, `a04`, `a05` | not started |
| 10 — Docs/CI/release policy | `f01` | not started |

## Decisions

- **2026-10-02 — Studio-over-USB is a critical product feature; the C3 cannot
  provide it.** The C3 has no USB OTG, and ZMK's Studio UART RPC transport is
  only selected when a ZMK USB *endpoint* is active (`app/src/studio/rpc.c`
  matches transports against `zmk_endpoint_get_selected().transport`; the UART
  transport registers as `ZMK_TRANSPORT_USB`). The USB Serial/JTAG console port
  is a plain UART device, not a ZMK endpoint, so it cannot carry Studio RPC
  without fork-level changes to the endpoint/RPC layer. Decision: the C3 stays
  BLE-only (Studio, if enabled, uses the BLE GATT transport, which is
  `default y` under `ZMK_BLE`); the Studio-over-USB requirement is carried by
  the ESP32-S3 in Phase 6/8 (USB OTG → `ZMK_USB` → CDC-ACM Studio port).
- **2026-10-02 — Phase 5 C3 work lives on branch `feat/esp32c3`** (branched
  from `feat/esp32` at "Close phase 4"). Phase 6 (S3) references this work as
  the ZMK-on-Espressif pattern: board-extension layout, shield/fixture
  structure, toolchain setup (cross-compile variant — the `espressif` variant
  was removed upstream in Zephyr 4.4), and the build-script conventions.

## Host test matrix

Repeatable procedures for the host OS matrix (Phase 0 task 6). Hardware
availability is recorded per host; where a host is unavailable the procedure
and a named owner are recorded instead.

All hosts are available; owner for every host: **Ryan**. Record the exact
device model and OS version in the first test-results entry per host
(`test-results/format.md` → Host OS field).

| Host OS | Hardware | Availability | Pairing / typing procedure | Owner |
| --- | --- | --- | --- | --- |
| Linux | CI runner + dev machine | available | `bluetoothctl`: `power on`, `agent on`, `default-agent`, `scan on`, `pair <mac>`, `trust <mac>`, `connect <mac>`; verify typing with `evtest`/`wev` or a text editor | Ryan |
| macOS | dev machine | available | System Settings → Bluetooth → pair; verify with any text field; `log stream --predicate 'subsystem == "com.apple.bluetooth"'` for diagnostics | Ryan |
| Windows | dev machine | available | Settings → Bluetooth & devices → add device; verify in Notepad; Event Viewer → Bluetooth support for diagnostics | Ryan |
| Android | phone | available | Settings → Bluetooth → pair; verify in a text field | Ryan |
| iOS | iPhone | available | Settings → Bluetooth → pair; verify in Notes | Ryan |
