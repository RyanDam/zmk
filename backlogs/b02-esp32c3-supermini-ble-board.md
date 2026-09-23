# b02 — Phase 5: Add ESP32-C3 SuperMini BLE board variant

- **Category:** b (Build, deploy & config)
- **Severity:** High
- **Status:** TODO
- **Effort (est):** M

## Problem
There is no ZMK board variant for the ESP32-C3 SuperMini. Zephyr 4.4 already ships the
upstream `esp32c3_supermini/esp32c3` board (4 MB flash, BOOT on GPIO9, active-low LED on
GPIO8, USB Serial/JTAG console, Espressif BT HCI node), but ZMK has no `zmk` extension,
defconfig, DTS, matrix fixture, or CI coverage for it.

## Evidence
- `report.md:69-74` — upstream C3 SuperMini board contents and pin reservations (GPIO18/19 = USB Serial/JTAG, GPIO9 = BOOT, GPIO8 = LED, strapping GPIO2/8/9).
- `implement_plan.md:231-277` — Phase 5 design, tasks, proposed paths, and functional tests.
- `report.md:15` — the C3 has fixed-function USB Serial/JTAG, not USB OTG; USB HID is not a C3 capability.

## Why it matters
- The C3 is the first ESP target and the lower-risk bring-up (single-core RISC-V, upstream
  board exists). It establishes the ZMK-on-Espressif pattern (defconfig/DTS/settings
  partition/HCI retention) that the S3 variant reuses.

## Conservative fix
- Add a ZMK board extension for the upstream board per the Zephyr 4.4 board-extension convention; do not redefine `board.yml` (upstream already declares board/SoC).
- Add the C3 ZMK defconfig: enable GPIO and `CONFIG_ZMK_BLE=y`; keep ZMK USB disabled; enable flash map/page layout and a candidate settings backend; retain normal Bluetooth host config and Espressif HCI selection; no guessed BT buffer/stack tuning before measurements.
- Add the C3 ZMK DTS extension: extend the upstream SuperMini DTS; preserve `zephyr,bt-hci = &esp32_bt_hci` and the enabled HCI node; add a non-overlapping writable storage partition sized for ZMK settings; disable/repurpose console/UART only if it conflicts with the matrix fixture; make flash-size/module assumptions explicit.
- Add a minimal direct-matrix shield/fixture configuration separate from the generic board variant; default pins must avoid GPIO18/19, GPIO8, GPIO9, and strapping GPIO2/8/9 — prefer GPIO0, GPIO1, GPIO3–GPIO5, GPIO10, GPIO20, GPIO21 subject to the qualified carrier schematic. Disable UART console if GPIO20/21 become matrix pins.
- Settings storage: first attempt NVS + `SETTINGS_NVS`; verify erased-flash boot, save/load across reset and power cycle, bond overwrite/clear, and repeated writes. Use FCB only as an explicitly documented temporary fallback with an upstream issue filed.
- Add C3 compile targets to CI; configure blob-backed hardware tests outside generic CI if secrets/licenses prohibit blob fetching there.

## Out of scope (for now)
- USB HID on C3 (hardware-impossible), battery-life claims, wireless split, RGB/display, deep sleep.

## Acceptance / verification
- [ ] Builds cleanly with blobs fetched; flashes via the Espressif runner; boot logs clean.
- [ ] Fixture matrix scans and sends keys over BLE.
- [ ] Pairing and typing verified on Linux, macOS, Windows, Android, and iOS as available.
- [ ] All ZMK profile operations pass: first pair, profile selection, reconnect, clear one bond, clear all bonds, settings reset, power-cycle persistence.
- [ ] Erased-flash first boot, 100+ repeated settings saves, unexpected reset during use, and reflash recovery pass.
- [ ] 24-hour connected idle and repeated reconnect tests pass with disconnect reasons logged.
- [ ] Enabling ZMK USB is rejected/unsupported in documentation and CI.
- [ ] Marked experimental until d03 power criteria are met.
