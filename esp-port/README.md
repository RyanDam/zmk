# ESP32-C3 / ESP32-S3 port — working records

Working records for the ZMK ESP32-C3/S3 SuperMini port (see
[`../implement_plan.md`](../implement_plan.md) for the phase plan and
[`../../backlogs/`](../../backlogs/) for tracked items).

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
| `build_esp32.sh`, `build_zmk_c3.sh`, `build_zmk_s3.sh` | Port build scripts (C3/S3 SuperMini fixtures) | 5, 6 |
| `build_cobantab1a.sh` | Coban Tab 1a build script (BLE + display + touch) | 9b |

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
| 8 — S3 USB HID (conditional) | `b04` | won't-do (superseded) — b06 (2026-11-25) proved wired USB HID + Studio CDC on tab1a **unfused** via runtime PHY switch; merged into the default `tab1a` build 2026-11-26. Only boot-mode/fused production questions remain open, should they ever matter |
| 9 — Optional capabilities | `a03`, `a04`, `a05` | not started |
| 9b — cobantab1a display module (Phase 9b display class) | `b05` | in progress (board def + AXS15231B display/touch drivers + shield compile-verified; hardware bring-up pending) |
| 10 — Docs/CI/release policy | `f01` | not started |

## Decisions

- **2026-11-25 — b06: Studio-over-USB + wired HID on tab1a need NO eFuse.**
  Deep-dive (`docs/b06-efuse-usb-phy-sel-report.md` in the workspace repo):
  `USB_PHY_SEL` only decides *boot-mode* pad ownership; at runtime the Zephyr
  DWC2-ESP UDC driver hands pads 19/20 from USB-Serial/JTAG to OTG via
  `usb_wrap` (`OTG_CONF.pad_enable`, already in the driver path on S3), and
  every reset reverts — recovery/flashing on an unfused chip stays
  Serial/JTAG, so OTG on tab1a is fully reversible (plan rev 2:
  zero eFuse writes allowed). The 2026-10-02 note's "Phase 6/8" plan is thus
  realized in software only; burning stays a b04 production question.
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
- **2026-11-14 — cobantab1a (b05) work lives on branch `feat/cobantab1a`**
  (zmk fork) and `feat/cobantab1a` (zmk-config), branched from `feat/esp32`
  and `master` respectively. The board def lives in the fork
  (`app/boards/others/cobantab1a/`); the shield (overlay/keymap/conf) lives in
  the external config repo (`boards/shields/cobantab1a/`), same as the
  cobanpad shields.
- **2026-11-14 — cobantab1a gets no CI entry.** CI has no access to the
  external ZMK config repo, and the coban product shields (cobanpad12b/16a,
  cobantab1a) live there — so none of them are in `app/core-coverage.yml`.
  (The `esp32s3_matrix_fixture` CI entry is a port *test fixture* that ships
  in the repo, not a product shield.) The canonical build path is
  `esp-port/build_cobantab1a.sh` (pristine build, `-DSHIELD=cobantab1a`,
  `ZMK_CONFIG` pointing at the config repo).
- **2026-11-14 — LVGL memory on the ESP32-S3 goes to PSRAM via the
  `.lvgl_buf`/`.lvgl_heap` custom sections**, not via `zephyr,memory-region`
  nodes: the ESP32-S3 linker script (`zephyr/soc/espressif/esp32s3/default.ld`)
  has fixed MEMORY regions and already `KEEP(*(.lvgl_buf*))` /
  `KEEP(*(.lvgl_heap*))` into `.ext_ram.data` (→ `ext_dram_seg` → PSRAM).
  Hence `CONFIG_LV_Z_VDB_CUSTOM_SECTION` + `CONFIG_LV_Z_MEMORY_POOL_CUSTOM_SECTION`
  in the shield conf. (Custom `zephyr,memory-region` nodes produce orphan
  sections that the ESP32 linker dumps into `IDT_LIST` and overflows.)

- **2026-11-26 — a15 asset uploads are commit-last, not transactional.** The
  writable store (`ZMKAS002`) appends blobs into free gaps and rewrites only
  the manifest sector (generation counter + trailing CRC32, per-blob CRC32)
  as the *last* act, so any interruption — abort, CRC mismatch, power loss —
  provably leaves the previous generation byte-identical; probe validates the
  manifest CRC before trusting anything. Upload runs on a dedicated
  preempt-low `k_work_q` (`asset_upload.c`): the RPC thread only copies
  chunks into a 16 KiB staging ring (decoded 1 KiB chunk structs + nanopb
  frames need the tab1a `ZMK_STUDIO_RPC_THREAD_STACK_SIZE=8192` bump), and
  blob relocation (GC) parks both anim players via a quiesce handshake on the
  200 ms tick before touching bytes. Resume is within-session (RAM state) by
  design; cross-reboot resume would need a journal for one failure mode the
  commit-last layout already defeats. Also fixed the a12 carve overlap (slot1
  ran 16 KiB past the assets start; assets covered `slot0_lpcore`) — assets
  is now exactly 2016 KiB @ `0xda8000`. Rationale + full design:
  workspace `docs/a15-bulk-asset-upload-plan.md`.
- **2026-11-26 — harness proto contract:** `studio-harness` loads protos live
  from `modules/msgs/` (protobufjs: fields camelCase, `fromObject` takes oneof
  *members* flat, virtual oneof wrappers are `toObject`-only, enum `GENERIC`
  is 0/falsy). The a14 harness encoded nested `{requestType:…}` (silently
  empty subsystems) and had a wrong `PROTO_DIR` — never caught because only
  the framing unit test ran pre-bench; both fixed, selftest now covers it.

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
