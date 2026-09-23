# b03 — Phase 6: Add ESP32-S3 SuperMini BLE board variant

- **Category:** b (Build, deploy & config)
- **Severity:** High
- **Status:** TODO
- **Effort (est):** L

## Problem
No upstream Zephyr 4.4 `esp32s3_supermini` board exists, and ZMK has no ESP32-S3 SuperMini
board or `zmk` variant. Because its memory topology, exposed pins, LED, and USB wiring
differ from a DevKitC, a dedicated custom board definition (not a DevKitC extension) is
required.

## Evidence
- `report.md:76-85` — S3 SuperMini pin reservations (GPIO19/20 = USB-C, GPIO0 = BOOT, strapping GPIO3/45/46, WS2812 on GPIO48, UART0 recovery on GPIO43/44, conservative matrix set GPIO1, GPIO2, GPIO4–8, GPIO15–18, GPIO21; exclude GPIO33–38 from defaults).
- `report.md:78` — common sample is `ESP32-S3FH4R2` (4 MB flash + 2 MB QSPI PSRAM); starting DTS include `espressif/esp32s3/esp32s3_wroom_n4r2.dtsi`, chip marking must be verified.
- `implement_plan.md:281-309` — Phase 6 tasks and proposed paths.

## Why it matters
- The S3 is the higher-capability ESP target (dual-core Xtensa, USB OTG hardware, more
  RAM). Using only the PROCPU target keeps the port single-core and avoids SMP/AMP
  complexity in the initial release.

## Conservative fix
- Add `board.yml` with `full_name`, board vendor/category, the `esp32s3` SoC, the `procpu` qualifier, and the ZMK variant.
- Add the base board DTS starting from `espressif/esp32s3/esp32s3_wroom_n4r2.dtsi` only after confirming the reference chip's 4 MB flash / 2 MB QSPI PSRAM topology; set flash/PSRAM properties explicitly and use an Espressif 4-MB partition include as the starting layout.
- Add S3 pinctrl and board-CMake/runner configuration based on current Zephyr S3 boards; configure USB Serial/JTAG console for normal development; preserve the Espressif HCI chosen node.
- Add the `zmk` variant defconfig and DTS: enable BLE/GPIO/settings, disable ZMK USB, create a dedicated writable settings partition, keep Wi-Fi disabled.
- Add an S3 direct matrix fixture: default pins GPIO1, GPIO2, GPIO4–GPIO8, GPIO15–GPIO18, GPIO21; reserve GPIO0, GPIO3, GPIO19/20, GPIO45/46, GPIO48; retain GPIO43/44 as UART recovery until USB behavior is qualified; do not use GPIO33–38 in the portable default fixture.
- Leave PSRAM unused by default; add PSRAM configuration only after dedicated memory/boot validation on the exact FH4R2 board.
- Add S3 BLE compilation to CI and hardware qualification to the test matrix.

## Out of scope (for now)
- USB HID (b04, gated on the USB-PHY/eFuse decision), SMP/AMP or APPCPU execution, PSRAM enablement, Wi-Fi.

## Acceptance / verification
- [ ] S3 board builds and flashes; PROCPU-only operation documented.
- [ ] S3 passes the same BLE functional and persistence suite as the C3 (b02 test list).
- [ ] S3 BLE compile target green in CI.
- [ ] PSRAM left unused by default in the shipped configuration.
- [ ] Qualified against a concrete physical revision (chip marking recorded); no compatibility claimed for untested clones.
