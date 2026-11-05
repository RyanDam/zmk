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
| 5 — C3 BLE board | `b02` | in progress (board + fixture merged to `feat/esp32`, hardware tests pending) |
| 6 — S3 BLE board | `b03` | in progress (board + fixture merged to `feat/esp32`, hardware tests pending) |
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
- **2026-10-03 — Phase 6 (S3) work lives on branch `feat/esp32s3`**,
  branched from `feat/esp32` at "Close phase 4". The Phase 5 C3 board is NOT
  in this branch, so the CI compile target covers the S3 only; the C3 compile
  target gets added when `feat/esp32c3` is merged. The S3 board reuses the
  ZMK-on-Espressif pattern established by the C3 work (board layout,
  shield/fixture structure, cross-compile toolchain setup, build-script
  conventions) without depending on its files.
- **2026-10-03 — ESP CI infrastructure lands with Phase 6.** The build
  workflow installs the Espressif 12.2.0 cross compilers (riscv32 + xtensa,
  both chips) and fetches the `hal_espressif` blobs for any matrix entry with
  `toolchain: espressif` — explicit in `app/core-coverage.yml`, or derived
  from the SoC in the board name for board-changes/nightly entries. Builds
  use the `cross-compile` toolchain variant (Zephyr 4.4 removed the legacy
  `espressif` variant); see `toolchain-esp32.md`.
- **2026-10-03 — S3 fixture keymap ships in the repo** at
  `app/boards/shields/esp32s3_matrix_fixture/esp32s3_matrix_fixture.keymap`
  because CI has no access to the external ZMK config repo and the build
  hard-fails without a keymap node. A copy in the config repo
  (`esp32s3_matrix_fixture.keymap`) takes precedence for local builds via
  `-DZMK_CONFIG`.
- **2026-11-05 — Phases 5 and 6 merged into `feat/esp32`.** The
  `feat/esp32s3` and `feat/esp32c3` lines (both branched from `feat/esp32` at
  "Close phase 4") were merged without fast-forward, so both boards and both
  matrix fixtures now live in one tree and the CI matrix in
  `app/core-coverage.yml` compiles the S3 and the C3 fixture.

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
