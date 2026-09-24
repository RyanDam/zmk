# Baseline build sizes — 2026-09-24

Supersedes `build-sizes-2026-09-23.md` (red baseline) after the c01 fix
(`indicator.c` LED devicetree guards) landed. Same representative build set:
upstream Nordic, RP2040, and ESP32 boards plus this fork's own shields.

Host: Linux container, Zephyr SDK 0.16.9, west v1.5.0, CMake 3.31.6,
Ninja 1.11.1. Branch `feat/esp32` @ `0c9ff4d8` + c01 fix (committed with
this baseline). All builds `west build --pristine` into fresh build dirs.

## Results

| Target | Board / shield / args | Result | FLASH | RAM | Image |
| --- | --- | --- | --- | --- | --- |
| cobanpad16a | `nice_nano@1` + `-S studio-rpc-usb-uart -S zmk-usb-logging` + `-DZMK_CONFIG=/workspaces/zmk-config -DSHIELD=cobanpad16a` | **PASS** | 320 896 B / 792 KB (39.57 %) | 110 908 B / 256 KB (42.31 %) | `zmk.uf2` 642 048 B (start 0x26000) |
| cobanpad12b | `nice_nano@1` + `-S studio-rpc-usb-uart -S zmk-usb-logging` + `-DZMK_CONFIG=/workspaces/zmk-config -DSHIELD=cobanpad12b` | **PASS** | 297 600 B / 792 KB (36.70 %) | 108 644 B / 256 KB (41.44 %) | `zmk.uf2` 595 456 B (start 0x26000) |
| corne-left | `nice_nano//zmk` + `-DSHIELD=corne_left` | **PASS** | 231 548 B / 792 KB (28.55 %) | 54 744 B / 256 KB (20.88 %) | `zmk.uf2` 463 360 B (start 0x26000) |
| reviung41 | `sparkfun_pro_micro_rp2040//zmk` + `-DSHIELD=reviung41` | **PASS** | 40 484 B / 1 572 608 B (2.57 %) (+ BOOT_FLASH 256 B / 256 B) | 17 332 B / 263 KB (6.44 %) | `zmk.uf2` 81 920 B (start 0x10000000) |
| bdn9 | `bdn9//zmk` | **PASS** | 35 100 B / 128 KB (26.78 %) | 9 976 B / 16 KB (60.89 %) | `zmk.bin` 35 100 B (no uf2 for this target) |

## Notes

- **cobanpad16a is byte-identical to the 2026-09-23 baseline** (FLASH
  320 896 B, RAM 110 908 B, uf2 642 048 B) — the c01 fix takes the exact
  same code path on the coban boards (their devicetree defines the LED
  hardware, so the guarded path compiles in unchanged).
- No compiler warnings in any of the five builds (verified by touching the
  changed sources and recompiling the affected translation units; the only
  diagnostic is a pre-existing CMake deprecation notice from
  `keymap-module` about the `KSCAN` symbol, present before the fix).
- `bdn9` and `cobanpad12b` previously failed only because of the test-suite
  `parse_syscalls` race (stale `app/build/tests/*` dirs wiped mid-build);
  both build cleanly when not run concurrently with the suite.
- The upstream CI build matrix (corne, romac, tidbit, nice60, planck,
  xiao_ble/hummingbird, bdn9, RP2040, …) is unblocked: the three
  representative upstream boards above all build. This table is the Phase 1
  size-regression reference; extend it per the d01 fixture list as hardware
  fixtures land.
