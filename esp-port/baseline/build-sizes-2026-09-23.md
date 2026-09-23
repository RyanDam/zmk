# Baseline build sizes — 2026-09-23

Representative build set for the Phase 1/3 size-regression reference
(Phase 1 validation list: representative USB+BLE Nordic, RP2040, STM32,
native, split boards — plus this fork's own board).

Host: Linux container, Zephyr SDK 0.16.9, west v1.5.0, CMake 3.31.6,
Ninja 1.11.1. Branch `feat/esp32` @ firmware state `ab902bf8`.
All builds `west build --pristine`.

## Results

| Target | Board / shield / args | Result | FLASH | RAM | Image |
| --- | --- | --- | --- | --- | --- |
| cobanpad16a | `nice_nano@1` + `-S studio-rpc-usb-uart -S zmk-usb-logging` + `-DZMK_CONFIG=/workspaces/zmk-config -DSHIELD=cobanpad16a` | **PASS** | 320 896 B / 792 KB (39.57 %) | 110 908 B / 256 KB (42.31 %) | `zmk.uf2` 642 048 B (start 0x26000) |
| corne-left | `nice_nano//zmk` + `-DSHIELD=corne_left` | **FAIL** (build) | — | — | — |
| reviung41 | `sparkfun_pro_micro_rp2040//zmk` + `-DSHIELD=reviung41` | **FAIL** (build) | — | — | — |
| bdn9 | `bdn9//zmk` | **FAIL** (build) | — | — | — |

## Failure cause (all three)

Same pre-existing `app/src/indicator.c` issue recorded in
`tests-2026-09-23.md` — the file is compiled unconditionally and requires
LED devicetree that upstream shields do not define:

- `corne-left`: `DT_N_ALIAS_led_l0_CHILD_IDX undeclared`
  (`corne_left` defines a `leds` node but no `led-l0..l3` aliases)
- `reviung41`, `bdn9`: `__device_dts_ord___ORD undeclared`
  (no `gpio_leds` node at all → `LED_GPIO_NODE_ID` = -1)

## Consequence

On this branch, **only boards/shields that define the coban LED hardware
build**. The upstream CI build matrix (corne, romac, tidbit, nice60, planck,
xiao_ble/hummingbird, bdn9, RP2040, …) is expected to be fully red until the
`indicator.c` guard lands. After the fix, re-run this table and record the
full size set as the Phase 1 reference.
