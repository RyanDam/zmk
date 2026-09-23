# c01 — indicator.c requires LED devicetree unconditionally, breaking all test and upstream board builds

- **Category:** c (Correctness & reliability)
- **Severity:** High
- **Status:** TODO
- **Effort (est):** S

## Problem
`app/src/indicator.c` is compiled into every ZMK build (no Kconfig guard) and
its non-LED-strip path hard-requires devicetree that only the coban boards
provide: a `gpio_leds` node and `led-l0`–`led-l3` aliases. Any target without
that devicetree — all 248 native test keymaps, and every upstream
board/shield (corne, romac, tidbit, nice60, planck, xiao_ble, bdn9, RP2040,
…) — fails to compile. This makes the entire native test suite and the
upstream CI build matrix red on `feat/esp32` and blocks the Phase 0 exit
gate ("current mainline builds and tests are green").

## Evidence
- `app/CMakeLists.txt:116` — `target_sources(app PRIVATE src/indicator.c)` is
  unconditional (contrast with the `target_sources_ifdef(...)` lines around
  it).
- `app/src/indicator.c:34` — `#define LED_GPIO_NODE_ID
  DT_COMPAT_GET_ANY_STATUS_OKAY(gpio_leds)` evaluates to `-1` when no
  `gpio_leds` node exists.
- `app/src/indicator.c:45` — `DEVICE_DT_GET(LED_GPIO_NODE_ID)` with `-1`
  expands to `__device_dts_ord___ORD` → compile error
  (`'__device_dts_ord___ORD' undeclared`).
- `app/src/indicator.c:46-48` — `led_idx[]` references
  `DT_NODE_CHILD_IDX(DT_ALIAS(led_l0..l3))` → compile error
  (`'DT_N_ALIAS_led_l0_CHILD_IDX' undeclared`) when the aliases are absent.
- `app/src/indicator.c:36-43` — the `BUILD_ASSERT`s documenting the LED
  requirement are commented out, so the requirement is neither enforced nor
  documented.
- `app/src/indicator.c:29` — the LED-strip path has the same shape of
  problem: `DEVICE_DT_GET(DT_CHOSEN(zmk_indicator_strip))` with no
  `DT_NODE_EXISTS` guard (only reached when
  `CONFIG_COBAN_INDICATOR_USE_LED_STRIP=y`).
- `esp-port/baseline/tests-2026-09-23.md` — measured: 248/248 native test
  targets fail to build (0 pass), uniform cause.
- `esp-port/baseline/build-sizes-2026-09-23.md` — measured: `corne_left`,
  `reviung41`, `bdn9` builds fail with this cause; only `cobanpad16a`
  (defines the LED hardware) builds.
- `backlogs/d01-esp-port-baseline-hardware-fixtures.md` — Phase 0 exit gate
  blocked by this failure.

## Why it matters
- No automated test can run on this branch, so no later phase (Zephyr 4.4.1
  upgrade, kscan/USB migration) has a green regression baseline.
- Every upstream board/shield is unbuildable on this branch — a silent
  support regression for all non-coban hardware.
- A compile-time hard dependency on one vendor's LED wiring, hidden behind
  commented-out asserts, will keep biting every new board/test added.

## Conservative fix
Make the LED path conditional on devicetree presence, keeping the file
compiled for all targets (no CMake/Kconfig restructuring):
- `app/src/indicator.c:34-48` — only define `led_dev`/`led_idx` when
  `DT_NODE_EXISTS(LED_GPIO_NODE_ID)` and the `led-l0`–`led-l3` aliases exist
  (e.g. wrap in `#if DT_NODE_EXISTS(...) && DT_NODE_EXISTS(DT_ALIAS(led_l0))
  ...`); otherwise provide a no-op path (empty `led_idx`, guarded
  `led_on`/`led_off` calls) so indicator state tracking still compiles and
  runs without LEDs.
- `app/src/indicator.c:29` — guard the LED-strip `strip_dev` on
  `DT_NODE_EXISTS(DT_CHOSEN(zmk_indicator_strip))` with the same no-op
  fallback, so `CONFIG_COBAN_INDICATOR_USE_LED_STRIP=y` without the chosen
  node fails loudly with a clear `BUILD_ASSERT` message instead of an
  opaque `__device_dts_ord___ORD` error.
- Restore the commented-out `BUILD_ASSERT`s (lines 36-43) in a form that
  only applies when the LED path is active, documenting the requirement.
- Do **not** change indicator behavior on `cobanpad16a`/`cobanpad12b` —
  their devicetree keeps taking the same code path.

## Out of scope (for now)
- Moving `indicator.c` behind a new Kconfig option (larger restructuring;
  revisit if the no-op path proves awkward).
- Any change to indicator colors, timing, or the WS2812 strip behavior.
- Re-running the full Phase 0 baseline — that is a follow-up step after this
  fix lands (refresh `esp-port/baseline/tests-*.md` and
  `build-sizes-*.md`), tracked by `d01`.

## Acceptance / verification
- [ ] `cd app && ./run-test.sh all` → all 248 targets build; suite result
      matches snapshots (no new failures, no snapshot changes).
- [ ] `corne_left` (nice_nano), `reviung41` (sparkfun_pro_micro_rp2040),
      `bdn9` and `cobanpad16a` (nice_nano@1) all build cleanly with
      `west build --pristine`.
- [ ] `cobanpad16a` LED indicator behavior unchanged (physical check:
      layer/indicator LEDs respond as before).
- [ ] No new compiler warnings in the affected targets.
- [ ] `esp-port/baseline/tests-2026-09-23.md` and
      `esp-port/baseline/build-sizes-2026-09-23.md` refreshed (or new dated
      files added) showing the green baseline.
