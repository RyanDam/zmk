# 05-001 — C3 ZMK board compile gate

- Date: 2026-10-02
- Test case: build-compile
- Result: PASS
- Duration: ~4m (pristine, both modes)

## Environment

- Board: n/a (build-only)
- Board/module revision: n/a
- Chip marking: n/a
- Flash / PSRAM: 4 MB flash / no PSRAM
- USB-PHY / eFuse state: n/a
- Firmware: RyanDam/zmk @ `1730de8a3e9e1bda18c7696581895129d267f776`
  (branch `feat/esp32`, plus uncommitted Phase 5 board/shield/keymap files),
  target `esp32c3_supermini/esp32c3/zmk` + shield `esp32c3_matrix_fixture`,
  image SHA-256 `159d7ac6a3bdb6a76201feeee3c7ef2cf920af073840f67efdcc40d204037909`
  (blob-backed `zmk.bin`)
- Zephyr: `8789b5b8cb5c1668de65953b94c8c4586d79859a`
  (`v4.4.1+zmk-fixes`); hal_espressif: `f07465053c0f24aa254d9fdfd559f06879221073`
- Toolchain: Espressif riscv32-esp-elf GCC 12.2.0 (12.2.0_20230208) via
  `ZEPHYR_TOOLCHAIN_VARIANT=cross-compile` (the legacy `espressif` variant was
  removed upstream in Zephyr 4.4 — see `../toolchain-esp32.md`), west v1.5.0,
  Python 3.12.3, esptool 5.4.0
- Host OS: Linux (container, aarch64)
- Power: n/a

## Procedure

1. `sh esp-port/build_zmk_c3.sh --no-blobs`
   (`CONFIG_BUILD_ONLY_NO_BLOBS=y`, compile-only).
2. `west blobs fetch hal_espressif`.
3. `sh esp-port/build_zmk_c3.sh` (full build with blobs).

## Observations / measurements

- Both builds complete cleanly (exit 0).
- Compile-only: FLASH 209588 B (5.00% of 4 MB); BT HCI driver stubbed with the
  expected "Building only the Bluetooth driver without binary blobs" warning.
- Blob-backed: FLASH 414436 B (9.88%), iram0 56160 B (14.83%),
  dram0 136672 B (36.10%); no blob warnings; esptool image generated.
- Kconfig as built: `CONFIG_ZMK_BLE=y`, `CONFIG_BT_ESP32=y` (auto-selected from
  the `espressif,esp32-bt-hci` DT node), `CONFIG_NVS=y`,
  `CONFIG_SETTINGS_NVS=y`, `CONFIG_ZMK_USB` unset.
- Devicetree resolves: board extension
  `app/boards/others/esp32c3_supermini/esp32c3_supermini_esp32c3_zmk.dts`,
  shield overlay `app/boards/shields/esp32c3_matrix_fixture/esp32c3_matrix_fixture.overlay`,
  keymap `/workspaces/zmk-config/esp32c3_matrix_fixture.keymap` (found via
  `-DZMK_CONFIG`).
- Warnings: only pre-existing upstream ones (RWX LOAD segment, unused
  behavior driver stubs, deprecated `config/boards` deprecation notice).

## Artifacts

- logs: `/tmp/opencode/zmk-c3-build.log` (compile-only),
  `/tmp/opencode/zmk-c3-build-full.log` (blob-backed) — ephemeral, re-run the
  script to reproduce
- image: `build/c3-zmk/zephyr/zmk.bin` (gitignored build dir)

## Notes / follow-ups

- Next: flash to the C3 SuperMini + matrix fixture and run the Phase 5
  functional tests (boot, matrix scan over BLE, pairing on 5 hosts, profile
  ops, NVS durability, 24 h soak) — see `backlogs/b02-esp32c3-supermini-ble-board.md`.
- C3 stays marked experimental until Phase 7 (d03) power criteria are met.
