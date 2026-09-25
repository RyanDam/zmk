# Reference board documentation

One file per **physical board**, named
`<chip>-supermini-<vendor>-<serial-or-lot>.md`. "SuperMini" is a third-party
form factor with vendor variation — the filename and the records below are
the hardware identifier; the bare name "SuperMini" is never sufficient.

Copy the template below for each board. Photographs go in a `photos/`
subdirectory next to the file (top view, bottom view, chip close-up, USB-C
close-up).

## Template

```markdown
# <Chip> SuperMini — <vendor> <lot/serial>

- Acquired: YYYY-MM-DD
- Target marking: <e.g. ESP32-C3FN4 / ESP32-S3FH4R2>
- Actual chip marking: <photo + full transcription, incl. tracking code>
- PCB revision: <silkscreen marking, photo>
- Flash: <size, marking>
- PSRAM: <size + type (QSPI/OPI), marking, or "none">

## Photos

- top: `photos/top.jpg`
- bottom: `photos/bottom.jpg`
- chip: `photos/chip.jpg`
- usb-c: `photos/usb-c.jpg`

## Wiring as found

| Signal | Net / GPIO | Evidence (continuity / schematic / boot log) |
| --- | --- | --- |
| USB-C D- | | |
| USB-C D+ | | |
| Onboard LED | | |
| BOOT button | | |
| RESET button | | |
| Battery pads (if present) | | |

## Deviations from the target reference

- <e.g. "PSRAM missing despite FH4R2 marking — reorder">

## Notes

- <anything a future developer must know>
```

## Boards in this set

Two units per chip: one is the qualified reference, the second is the
spare/sacrificial unit (Phase 8 eFuse work). Vendor/lot is recorded at
intake; rename the file if a specific vendor or lot is identified.

| Board | File | Chip marking (as reported) | Status |
| --- | --- | --- | --- |
| C3 #1 | [`esp32c3-supermini-generic-01.md`](esp32c3-supermini-generic-01.md) | `FN4P` | acquired — intake pending |
| C3 #2 | [`esp32c3-supermini-generic-02.md`](esp32c3-supermini-generic-02.md) | `FN4P` | acquired — intake pending |
| S3 #1 | [`esp32s3-supermini-generic-01.md`](esp32s3-supermini-generic-01.md) | `FH4R2P` | acquired — intake pending |
| S3 #2 | [`esp32s3-supermini-generic-02.md`](esp32s3-supermini-generic-02.md) | `FH4R2P` | acquired — intake pending |

## Known board information

Expected values for the common "SuperMini" revisions, gathered from public
sources on 2026-09-25. These are the *expected* values; each per-board file
records what was actually measured on that unit. Where a value is
revision-dependent it is flagged and must be confirmed at intake.

Sources: Espressif ESP32-C3 Series Datasheet v2.4, Espressif ESP32-S3 Series
Datasheet v2.2, Espressif ESP-Packaging chip-marking docs, the C3 SuperMini
board datasheet (PDF), the Zephyr `esp32c3_supermini` board definition
(`boards/others/`), espboards.dev, esp32.co.uk, and the
`UnsignedArduino/ESP32-S3-Super-Mini-Test` repo.

### ESP32-C3 SuperMini (target chip: ESP32-C3FN4)

- Chip: **ESP32-C3FN4** — 4 MB in-package flash, no PSRAM, QFN32 (5×5 mm).
  The board datasheet's "Chip model" line reads `ESP32C3FN4`. Note C3FN4 is
  **NRND** (not recommended for new designs); the successor is C3FH4
  (−40~105 °C vs −40~85 °C — the only difference). Fine for dev boards.
- CPU: RISC-V single-core, up to 160 MHz; 400 KB SRAM, 384 KB ROM.
- Radio: Wi-Fi 802.11 b/g/n + Bluetooth 5 (LE). Deep sleep ≈ 43 µA.
- Size: 22.52 × 18 mm, 16 pins @ 2.54 mm. 13 GPIOs exposed: 0–10, 20, 21.
- USB-C: **native USB Serial/JTAG** on GPIO18 (D-) / GPIO19 (D+). No
  bridge chip; the pins are not on the header. Flashing, console, and
  recovery all use this path.
- UART0: GPIO21 (TX) / GPIO20 (RX) — the exposed `TX`/`RX` pads.
- LEDs:
  - Blue user LED: **GPIO8, active-low** (Zephyr `esp32c3_supermini` dts).
  - Red power LED: on whenever powered; not GPIO-controllable.
- Buttons: BOOT = **GPIO9**; RESET = chip `EN`.
- Strapping pins (datasheet v2.4): GPIO2 (floating, no boot-mode function),
  GPIO8 (floating, ROM-message control), GPIO9 (weak pull-up; 0 = download
  mode, 1 = SPI boot). **GPIO0 is not a C3 strapping pin.**
- Chip pin functions relevant to the fixture (datasheet v2.4):
  - GPIO0 / GPIO1 = 32 kHz crystal pads (XTAL_32K_P/N). The common
    SuperMini revision does not fit a 32 kHz crystal, so these are free
    GPIOs — verify at intake.
  - GPIO4–7 = external JTAG (MTMS/MTDI/MTCK/MTDO). Usable as GPIO; doing so
    disables the *external* JTAG path only — USB Serial/JTAG is unaffected.
  - GPIO12–17 = in-package flash SPI (not exposed on the board).
- Silkscreen defaults (Arduino-style): I2C SDA=GPIO8 / SCL=GPIO9; SPI
  SCK=GPIO4 / MISO=GPIO5 / MOSI=GPIO6 / SS=GPIO7.
  - **Zephyr quirk:** the `esp32c3_supermini` board maps its `i2c-0` alias
    to SDA=GPIO9 / SCL=GPIO10, which does *not* match the silkscreen. If
    I2C is used, follow the devicetree, not the silkscreen.
- Antenna: CrossAir CA-C03 ceramic. Known-weak Wi-Fi range (layout issue,
  not the chip). Newer revisions / the red "SuperMini Plus" improve it.
- External power: 3.3–6 V on the `5V` pin, mutually exclusive with USB.
- Zephyr: `esp32c3_supermini` exists from Zephyr 4.4 (`boards/others/`);
  console = USB serial, UART0 disabled. On the 4.1 baseline use
  `esp32c3_devkitc/esp32c3` (pin-compatible for bring-up).

### ESP32-S3 SuperMini (target chip: ESP32-S3FH4R2)

- Chip: **ESP32-S3FH4R2** — 4 MB in-package flash + 2 MB **quad-SPI** PSRAM,
  QFN56 (7×7 mm). The "R2" is 2 MB QSPI PSRAM; enable it as *quad* mode,
  never octal. PSRAM is **disabled by default** in most toolchains.
- CPU: dual-core Xtensa LX7, up to 240 MHz; 512 KB SRAM, 384 KB ROM.
- Radio: Wi-Fi 802.11 b/g/n + Bluetooth 5 (LE). Deep sleep ≈ 43 µA.
- Size: ≈ 23.5 × 18 mm (revision-dependent), 38 pins: 5V/GND/3V3 + 35 GPIO.
  20 of the pins (GPIO14–18, 21, 33–42, 45–48) are **back-side castellated
  pads**, not side headers — solder directly.
- USB-C: **native USB** — GPIO19 (D-) / GPIO20 (D+), not on the header. No
  bridge chip. USB Serial/JTAG and USB OTG share the internal PHY, so
  OTG/TinyUSB firmware changes how the connector behaves (Phase 8).
- UART0: GPIO43 (TX) / GPIO44 (RX) — the exposed `TX`/`RX` pads; kept free
  as the recovery console.
- LEDs:
  - **WS2812 (WS2818) RGB on GPIO48.**
  - A **red LED also on GPIO48** — shared net, not independently
    controllable from the WS2812.
  - Charge LED for the battery charger.
- Buttons: BOOT = **GPIO0**; RESET = chip `EN`. Download mode = GPIO0 low +
  GPIO46 low (the BOOT/RESET circuit handles this).
- Battery: B+/B− back pads + onboard Li charger. BOOST solder jumper:
  unbridged = 100 mA charge, bridged = 300 mA (only for cells ≥ 500 mAh).
- Strapping pins (datasheet v2.2): GPIO0 (weak pull-up), GPIO3 (floating),
  GPIO45 (weak pull-down), GPIO46 (weak pull-down). **GPIO2 is not an S3
  strapping pin.** GPIO48 is the JTAG-source-select pin (not in the
  strapping table) and the WS2812/red-LED net.
- In-package memory (datasheet v2.2, Table 2-14): flash + quad PSRAM consume
  **GPIO26–32** (not exposed on the board). **GPIO33–38 are free** on the
  FH4R2 quad config — the DQ4–DQ7/DQS pins (33–37) are octal-only and are
  not used by quad flash/PSRAM.
- GPIO39–42 = default external JTAG (MTCK/MTDO/MTDI/MTMS); usable as GPIO.
- GPIO47 = octal SPI differential clock; free on the 3.3 V quad config.
- Antenna: PCB/ceramic. **Some units ship with the antenna soldered
  backwards** (feed trace to the unconnected mounting pad) — check feed
  orientation at intake.
- Zephyr: no upstream S3 SuperMini board exists yet (only
  `esp32c3_supermini` and `esp32h2_supermini` in `boards/others/`). For
  Phase 0 bring-up use `esp32s3_devkitc/esp32s3`; a dedicated
  `esp32s3_supermini` board lands in Phase 6.

## Chip marking reference

The "P" seen in `FN4P` / `FH4R2P` is the start of the **tracking code**,
not a variant suffix. Per the Espressif ESP-Packaging chip-marking
convention, the marking lines are: product name, then the flash/PSRAM code
(`FN4` for C3, `FH4R2` for S3), then the date code + tracking information.
So these chips are standard **ESP32-C3FN4** and **ESP32-S3FH4R2**.

Transcribe the full tracking code at intake: its 2nd character encodes the
silicon revision.

| Chip | Tracking 2nd char → revision |
| --- | --- |
| ESP32-C3 | A=v0.0, B=v0.1, C=v0.2, D=v0.3, E=v0.4, H=v1.1 |
| ESP32-S3 | A=v0.0, B=v0.1, C=v0.2 |
