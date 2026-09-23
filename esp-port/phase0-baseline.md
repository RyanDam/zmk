# Phase 0 — recorded software baseline

Recorded: 2026-09-23.
Purpose: the reference point against which the Zephyr 4.4.1 upgrade (Phase 1)
and all later phases are diffed. **No manifest, firmware, or pin changes are
part of this record.**

## Tracking branch

- Branch: `feat/esp32` (created from `feat/coban-indicator`, which includes
  `main` at the time of the merge).
- ZMK commit (branch HEAD at recording): `ab902bf8174a31bea973ffdc7b79194077fd5faf`
- `main` at recording: `9ebbeff0a8b69a42f14aec022cdf16c7a107b9e0`
- Branch-specific work included in the baseline: LED indicator (incl. WS2812),
  dynamic macros, MPR121 touchpad, Kailh Choc encoder, GPIO key driver,
  multi-binding sensor keymaps, Studio sensor/touchpad/macro RPC, USB-first
  transport selection (see `FEATURES.MD`).

## Zephyr baseline

- Remote: `https://github.com/zmkfirmware/zephyr`
- Pinned revision (`app/west.yml`): `v4.1.0+zmk-fixes`
- Resolved SHA: `58a5874a446ace2893a196848282d271a551e512`
- Upstream base: `zephyrproject-rtos/zephyr` tag `v4.1.0` =
  `d46356ab689e70ade5c08da19381b8e6655968cc`
- Patch series: **37 commits** on top of upstream `v4.1.0`
  (`git -C zephyr log --oneline d46356ab689e70ade5c08da19381b8e6655968cc..58a5874a446ace2893a196848282d271a551e512`)

### Patch list (newest first)

| SHA | Date | Subject | Phase-1 classification seed |
| --- | --- | --- | --- |
| `58a5874a4` | 2026-03-15 | backport ls0xx serial VCOM inversion support to Zephyr 4.1 | needs port (display) |
| `7b33a2e53` | 2026-01-18 | drivers: display: ls0xx: add Kconfig setting VCOM thread priority for ls0xx | needs port (display) |
| `4d1489d5d` | 2025-09-23 | drivers: display: ls0xx: add support for serial VCOM inversion | needs port (display) |
| `ec3651699` | 2025-07-15 | drivers: flash: stm32g0: Implement option_bytes_write\|read API | check 4.4 (likely upstreamed) |
| `ec69ba712` | 2025-07-21 | drivers: clock: stm32c0: Add an option to enable CRS for HSI48 | check 4.4 (likely upstreamed) |
| `9f90a8dfb` | 2025-07-11 | dts: arm: st: c0: Add clk-hsi48 for stm32c071 SOC | check 4.4 (likely upstreamed) |
| `7e964a723` | 2025-07-10 | include: zephyr: dt-bindings: clock: Add HSI48 support STM32C071 | check 4.4 (likely upstreamed) |
| `bdf6790ea` | 2025-07-09 | include: zephyr: dt-bindings: Fix USB_SEL mask | check 4.4 (likely upstreamed) |
| `b5b4daac5` | 2025-04-02 | include: zephyr: dt-bindings: Add STM32C0 USB clock selection support | check 4.4 (likely upstreamed) |
| `cad0de364` | 2025-04-02 | dts: arm: st: c0: Add USB device node | check 4.4 (likely upstreamed) |
| `75c39cc7d` | 2025-12-16 | boards: boardsource: Add blok flash controller | check 4.4 (likely upstreamed) |
| `1c698ff90` | 2025-06-10 | drivers: spi: spi_pl022: disable the SSP before reconfiguring | check 4.4 (likely upstreamed) |
| `9f600baef` | 2025-12-09 | drivers: input: Add sleep-mode-enable property for Pinnacle | check 4.4 (input driver — relevant to Phase 2) |
| `3bb4355ba` | 2025-10-21 | twister: prefer 'fork' on POSIX to maintain pre-3.14 behavior | check 4.4 (test infra) |
| `c7bb73bfe` | 2025-09-12 | tests: drivers: retained_mem: add BBRAM test. | check 4.4 (tests) |
| `f13abe5f1` | 2025-09-12 | tests: drivers: retained_mem: fix compilation. | check 4.4 (tests) |
| `47f69443f` | 2025-09-12 | drivers: retained_mem: add BBRAM driver. | check 4.4 (likely upstreamed) |
| `933505207` | 2025-11-17 | drivers: input: Support invert x/y in rel mode | check 4.4 (input driver — relevant to Phase 2) |
| `61e486680` | 2025-11-01 | boards: seeed: xiao_ble: UF2 runner default, remove ID match | check 4.4 (board) |
| `a74258944` | 2025-10-28 | input: pinnacle: Perform software reset on init | check 4.4 (input driver — relevant to Phase 2) |
| `4fcff76d0` | 2024-12-02 | soc: arm: stm32: Add ROM bootloader support | check 4.4 (likely upstreamed) |
| `5253a2411` | 2025-10-07 | retention: Skip mutex usage when in pre-kernel | check 4.4 (likely upstreamed) |
| `f69b56b65` | 2025-10-01 | arm: stm32: Add DTS describing ROM bootloader for f411 | check 4.4 (likely upstreamed) |
| `db74e1e58` | 2025-09-30 | boards: st: Add boot mode retention support to Nucleo WB55RG board | check 4.4 (likely upstreamed) |
| `20fbf5810` | 2025-09-30 | dts: arm: st: Add STM32WB bootloader information | check 4.4 (likely upstreamed) |
| `cec120e6a` | 2024-12-02 | arm: stm32: Add DTS describing ROM bootloader for f0 | check 4.4 (likely upstreamed) |
| `8ec72ab09` | 2025-09-30 | soc: raspberrypi: rpi_pico: Add RP2 bootloader support | check 4.4 (likely upstreamed) |
| `6f410790f` | 2025-08-31 | boards: Add Adafruit Metro RP2040 | check 4.4 (likely upstreamed) |
| `c4469341b` | 2025-08-15 | boards: seeed: Fix XIAO MG24 standard uart pins | check 4.4 (likely upstreamed) |
| `36ba1079b` | 2022-06-03 | boards: arm: Add BoardSource blok RP2040 board. | check 4.4 (likely upstreamed) |
| `14138d3be` | 2025-03-12 | tests: kernel: sleep: Add Silabs adjustment to max limit | check 4.4 (likely upstreamed) |
| `95098f069` | 2025-03-12 | drivers: timer: silabs: Fix calculation of next tick | check 4.4 (likely upstreamed) |
| `1a3d084ba` | 2025-03-12 | soc: silabs: Use configdefault for default values | check 4.4 (likely upstreamed) |
| `ec305a460` | 2025-04-08 | boards: seeed: Add XIAO MG24 | check 4.4 (likely upstreamed) |
| `f50faa7bc` | 2025-06-04 | cmake: modules: Add new post_boards_shields extension | check 4.4 (build system) |
| `eed722fee` | 2025-03-13 | Bluetooth: Controller: Fix connection update interval_us variables | check 4.4 (BT — relevant to Phase 5/6/7) |
| `ac2504c91` | 2025-04-01 | soc: nordic: Allow disabling binding header validation | check 4.4 (likely upstreamed) |

Classification seeds are starting points only; Phase 1 must verify each
against the Zephyr 4.4.1 tree and record the final disposition.

## West module revisions (resolved)

Recorded with `west list` from a `west update`-ed workspace on 2026-09-23.

| Project | Path | Revision | Remote |
| --- | --- | --- | --- |
| manifest | `app` | `HEAD` | — |
| zephyr | `zephyr` | `v4.1.0+zmk-fixes` (`58a5874a446a`) | zmkfirmware |
| hal_stm32 | `modules/hal/stm32` | `4fcc3a3f32abe1c4cb76d9d1cef967728dd03908` | zmkfirmware |
| lvgl | `modules/lib/gui/lvgl` | `f1db87ee98f1810328a8419572fa42a3b5f352ae` | zmkfirmware |
| zmk-studio-messages | `modules/msgs/zmk-studio-messages` | `main` | cobanfirmware (RyanDam) |
| canopennode | `modules/lib/canopennode` | `dec12fa3f0d790cafa8414a4c2930ea71ab72ffd` | zephyrproject-rtos |
| chre | `modules/lib/chre` | `3b32c76efee705af146124fb4190f71be5a4e36e` | zephyrproject-rtos |
| lz4 | `modules/lib/lz4` | `11b8a1e22fa651b524494e55d22b69d3d9cebcfd` | zephyrproject-rtos |
| nanopb | `modules/lib/nanopb` | `7307ce399b81ddcb3c3a5dc862c52d4754328d38` | zephyrproject-rtos |
| psa-arch-tests | `modules/tee/tf-m/psa-arch-tests` | `2cadb02a72eacda7042505dcbdd492371e8ce024` | zephyrproject-rtos |
| sof | `modules/audio/sof` | `bc08c9c606324cfba0c104f4ffaf5dd456cb11d6` | zephyrproject-rtos |
| tf-m-tests | `modules/tee/tf-m/tf-m-tests` | `502ea90105ee18f20c78f710e2ba2ded0fc0756e` | zephyrproject-rtos |
| tflite-micro | `optional/modules/lib/tflite-micro` | `8d404de73acf7687831e16d88e86e4f73cfddf8e` | zephyrproject-rtos |
| thrift | `optional/modules/lib/thrift` | `10023645a0e6cb7ce23fcd7fd3dbac9f18df6234` | zephyrproject-rtos |
| zephyr-lang-rust | `modules/lang/rust` | `37dc7fac3fb0372bc0e78e022bef87fcce68c48d` | zephyrproject-rtos |
| zscilib | `modules/lib/zscilib` | `ee1b287d9dd07208d2cc52284240ac25bb66eae3` | zephyrproject-rtos |
| acpica | `modules/lib/acpica` | `8d24867bc9c9d81c81eeac59391cda59333affd4` | zephyrproject-rtos |
| cmsis | `modules/hal/cmsis` | `d1b8b20b6278615b00e136374540eb1c00dcabe7` | zephyrproject-rtos |
| cmsis-dsp | `modules/lib/cmsis-dsp` | `d80a49b2bb186317dc1db4ac88da49c0ab77e6e7` | zephyrproject-rtos |
| cmsis-nn | `modules/lib/cmsis-nn` | `e9328d612ea3ea7d0d210d3ac16ea8667c01abdd` | zephyrproject-rtos |
| cmsis_6 | `modules/lib/cmsis_6` | `783317a3072554acbac86cca2ff24928cbf98d30` | zephyrproject-rtos |
| fatfs | `modules/fs/fatfs` | `16245c7c41d2b79e74984f49b5202551786b8a9b` | zephyrproject-rtos |
| hal_adi | `modules/hal/adi` | `633fcecf3717aaa22079cf6121627a879f24df51` | zephyrproject-rtos |
| hal_ambiq | `modules/hal/ambiq` | `87a188b91aca22ce3ce7deb4a1cbf7780d784673` | zephyrproject-rtos |
| hal_atmel | `modules/hal/atmel` | `da767444cce3c1d9ccd6b8a35fd7c67dc82d489c` | zephyrproject-rtos |
| hal_espressif | `modules/hal/espressif` | `202c59552dc98e5cd02386313e1977ecb17a131f` | zephyrproject-rtos |
| hal_ethos_u | `modules/hal/ethos_u` | `50ddffca1cc700112f25ad9bc077915a0355ee5d` | zephyrproject-rtos |
| hal_gigadevice | `modules/hal/gigadevice` | `2994b7dde8b0b0fa9b9c0ccb13474b6a486cddc3` | zephyrproject-rtos |
| hal_intel | `modules/hal/intel` | `0355bb816263c54eed23c7781034447af5d8200c` | zephyrproject-rtos |
| hal_nordic | `modules/hal/nordic` | `37ca068d7b013fb65a2acc9306bffa48a3e72839` | zephyrproject-rtos |
| hal_nuvoton | `modules/hal/nuvoton` | `466c3eed9c98453fb23953bf0e0427fea01924be` | zephyrproject-rtos |
| hal_quicklogic | `modules/hal/quicklogic` | `bad894440fe72c814864798c8e3a76d13edffb6c` | zephyrproject-rtos |
| hal_renesas | `modules/hal/renesas` | `3204903bdc5eda6869a40363560a69369c8d0e22` | zephyrproject-rtos |
| hal_rpi_pico | `modules/hal/rpi_pico` | `7b57b24588797e6e7bf18b6bda168e6b96374264` | zephyrproject-rtos |
| hal_silabs | `modules/hal/silabs` | `8a173e9e566a396a19d18da4661cb54ce098f268` | zephyrproject-rtos |
| hal_tdk | `modules/hal/tdk` | `6727477af1e46fa43878102489b9672a9d24e39f` | zephyrproject-rtos |
| hal_telink | `modules/hal/telink` | `4226c7fc17d5a34e557d026d428fc766191a0800` | zephyrproject-rtos |
| hal_wch | `modules/hal/wch` | `1de9d3e406726702ce7cfc504509a02ecc463554` | zephyrproject-rtos |
| hal_wurthelektronik | `modules/hal/wurthelektronik` | `e3e2797b224fc48fdef1bc3e5a12a7c73108bba2` | zephyrproject-rtos |
| hostap | `modules/lib/hostap` | `697fd2cf5cbbd0c5375fc34761b6a9d7489a67d2` | zephyrproject-rtos |
| liblc3 | `modules/lib/liblc3` | `48bbd3eacd36e99a57317a0a4867002e0b09e183` | zephyrproject-rtos |
| libmctp | `modules/lib/libmctp` | `b97860e78998551af99931ece149eeffc538bdb1` | zephyrproject-rtos |
| libmetal | `modules/hal/libmetal` | `3e8781aae9d7285203118c05bc01d4eb0ca565a7` | zephyrproject-rtos |
| littlefs | `modules/fs/littlefs` | `ed0531d59ee37f5fb2762bcf2fc8ba4efaf82656` | zephyrproject-rtos |
| mbedtls | `modules/crypto/mbedtls` | `4952e1328529ee549d412b498ea71c54f30aa3b1` | zephyrproject-rtos |
| mipi-sys-t | `modules/debug/mipi-sys-t` | `33e5c23cbedda5ba12dbe50c4baefb362a791001` | zephyrproject-rtos |
| nrf_hw_models | `modules/bsim_hw_models/nrf_hw_models` | `73a5d5827a94820be65b7d276d28173ec10bab9f` | zephyrproject-rtos |
| nrf_wifi | `modules/lib/nrf_wifi` | `e35f707a782b7c4c0eb83a3b06ca4e6eb693f29f` | zephyrproject-rtos |
| open-amp | `modules/lib/open-amp` | `52bb1783521c62c019451cee9b05b8eda9d7425f` | zephyrproject-rtos |
| percepio | `modules/debug/percepio` | `49e6dc202aa38c2a3edbafcc2dab85dec6aee973` | zephyrproject-rtos |
| picolibc | `modules/lib/picolibc` | `82d62ed1ac55b4e34a12d0390aced2dc9af13fc9` | zephyrproject-rtos |
| segger | `modules/debug/segger` | `cf56b1d9c80f81a26e2ac5727c9cf177116a4692` | zephyrproject-rtos |
| tinycrypt | `modules/crypto/tinycrypt` | `1012a3ebee18c15ede5efc8332ee2fc37817670f` | zephyrproject-rtos |
| trusted-firmware-a | `modules/tee/tf-a/trusted-firmware-a` | `713ffbf96c5bcbdeab757423f10f73eb304eff07` | zephyrproject-rtos |
| uoscore-uedhoc | `modules/lib/uoscore-uedhoc` | `54abc109c9c0adfd53c70077744c14e454f04f4a` | zephyrproject-rtos |
| zcbor | `modules/lib/zcbor` | `9b07780aca6fb21f82a241ba386ad9b379809337` | zephyrproject-rtos |

## Toolchain (local baseline host)

| Tool | Version |
| --- | --- |
| Zephyr SDK | 0.16.9 (`/opt/zephyr-sdk-0.16.9`, arm gcc 12.2.0) |
| west | v1.5.0 |
| Python | 3.12.3 |
| CMake | 3.31.6 |
| Ninja | 1.11.1 |
| CI container | `docker.io/zmkfirmware/zmk-build-arm:4.1` |

## Baseline build & test state

See `baseline/` for archived results:

- `baseline/tests-2026-09-23.md` — full native test-suite run on `feat/esp32`.
- `baseline/build-sizes-2026-09-23.md` — representative board build sizes.

### Known pre-existing failure (blocks the Phase 0 exit gate)

**Neither the native test suite nor the upstream board builds work on
`feat/esp32`.** `app/src/indicator.c` — a file that exists **only on this
branch** (absent in `main`) — is compiled unconditionally
(`app/CMakeLists.txt:116`, no Kconfig guard). Its non-LED-strip `#else` path
requires devicetree that test keymaps and upstream shields do not provide:

```
error: 'DT_N_ALIAS_led_l0_CHILD_IDX' undeclared here (not in a function)   # indicator.c:47, DT_ALIAS(led_l0)
error: '__device_dts_ord___ORD' undeclared here (not in a function)        # DEVICE_DT_GET(LED_GPIO_NODE_ID) with no gpio_leds node
```

The `BUILD_ASSERT`s documenting the LED requirement are commented out
(`indicator.c:35-40`).

Measured impact (2026-09-23, details in `baseline/`):

- Native test suite: **248/248 targets fail to build** (244 confirmed,
  4 interrupted, 0 pass) — `baseline/tests-2026-09-23.md`.
- Board builds: `corne_left`, `reviung41` (RP2040), `bdn9` (STM32) all fail
  with the same cause; only `cobanpad16a` (defines the LED hardware) builds —
  `baseline/build-sizes-2026-09-23.md`.
- Consequence: the upstream CI build matrix is expected to be fully red on
  this branch until the fix lands — `baseline/ci-2026-09-23.md`.

- Disposition: fix as a **separate small PR** (guard the `#else` path on
  `DT_NODE_EXISTS(DT_ALIAS(led_lN))` / `DT_NODE_EXISTS(LED_GPIO_NODE_ID)`,
  or provide `-1` fallbacks with runtime checks) — Phase 0 records the red
  state rather than masking it.
- Re-run the suite and the build-size table after the fix and refresh
  `baseline/tests-*.md` / `baseline/build-sizes-*.md` as the true green
  baseline before Phase 1 starts.
