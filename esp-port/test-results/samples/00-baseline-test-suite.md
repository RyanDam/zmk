# 00 — baseline-test-suite

- Date: 2026-09-23
- Test case: baseline-test-suite
- Result: FAIL
- Duration: ~50m (248 targets, J=4)

## Environment

- Board: n/a (host-side native_sim test suite)
- Board/module revision: n/a
- Chip marking: n/a
- Flash / PSRAM: n/a
- USB-PHY / eFuse state: n/a
- Firmware: RyanDam/zmk @ `ab902bf8174a31bea973ffdc7b79194077fd5faf`
  (branch `feat/esp32`), target `native_sim//zmk_test_mock`, image SHA-256 n/a
  (no target built)
- Zephyr: `58a5874a446ace2893a196848282d271a551e512`
  (`zmkfirmware/zephyr @ v4.1.0+zmk-fixes`); hal_espressif:
  `202c59552dc98e5cd02386313e1977ecb17a131f`
- Toolchain: Zephyr SDK 0.16.9, west v1.5.0, Python 3.12.3, CMake 3.31.6,
  Ninja 1.11.1
- Host OS: Linux (container, aarch64, 10 cores / 7 GB)
- Power: n/a

## Procedure

1. `cd app && ./run-test.sh all` (J=4).
2. Audit every target's `app/build/tests/<case>/build.log` and check for
   `zephyr/zmk.exe`.

## Observations / measurements

- 248 targets: **0 built, 244 confirmed build-failed, 4 interrupted** by a
  concurrent orphaned suite (killed mid-run; see
  `../../baseline/tests-2026-09-23.md` for the data-quality note).
- Uniform failure: `app/src/indicator.c:47` —
  `DT_N_ALIAS_led_l0_CHILD_IDX undeclared` (branch-only file compiled
  unconditionally; test keymaps define no `led-l0..l3` aliases / `gpio_leds`
  node).
- Measured current: n/a

## Artifacts

- logs: `../../baseline/tests-2026-09-23.md` (per-target logs under
  `app/build/tests/`, gitignored)
- photos: n/a

## Notes / follow-ups

- Blocks the Phase 0 exit gate ("mainline builds and tests green").
- Fix in a separate PR: guard the `#else` path of `indicator.c` on
  `DT_NODE_EXISTS(...)`; then re-run and record the green baseline.
