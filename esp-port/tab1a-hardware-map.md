# Coban Tab 1a — hardware map (backlog h01)

Authoritative pin/allocation record for **cobantab1a**, prototype (JC3248W535EN
dev board, "unit tab-01") and final board. The final board **reuses this exact
pinout** (program decision, `docs/tab1a-dev-plan.md` 2026-11-21), so h02–h04,
p01 and the d04 final-board checklist all consume this file — do not re-derive
pins elsewhere.

Sources: `ref/JC3248W535EN/README.md:64-88` (pin tables),
`ref/JC3248W535EN/docs/GPIO_pinout.jpg`, `docs/ESP32_schematic.jpg`,
`docs/Power_schematic.jpg`; existing claims verified against
`zmk-config/boards/shields/cobantab1a/cobantab1a.overlay` and
`cobantab1a-layout.dtsi`.

## Board identity

| | |
|---|---|
| SoM | JC3248W535EN-S3 — ESP32-S3 N16R8 (16 MB flash, 8 MB OPI PSRAM) |
| Zephyr board | `cobantab1a/esp32s3/procpu/zmk` (`zmk/app/boards/others/cobantab1a/`) |
| Panel | AXS15231B 320×480 QSPI + capacitive touch (I2C `0x3B`) — bring-up done, b05 |
| USB | native USB-Serial/JTAG on 19/20 (boot loader + flashing). Since b06 merged (2026-11-26) every tab1a image hands 19/20 to the OTG controller while running (HID + Studio CDC) — runtime `usb_wrap` switch, reverts at reset, no eFuse |

## GPIO allocation (sorted)

| GPIO | Function | Status | Notes / traps |
|---:|---|---|---|
| 1 | Panel backlight (LEDC0_CH0) | **used** | PWM; `cobantab1a.overlay:95-100,217-227` |
| 2 | I2S0 LRCLK (`I2S0_O_WS_GPIO2`) → speaker amp | reserved (h03) | was phantom-kscan row until h01 moved it |
| 3 | — | **do not use** | strapping (JTAG source select) |
| 4 | Touch I2C SDA | **used** | `I2C0_SDA_GPIO4`, open-drain+pull-up is load-bearing (`overlay:84-86`) |
| 5 | VBAT ADC (`ADC1_GPIO5_CHANNEL` = **ch 4**) | reserved (p01) | **never a key** (R8). VBAT divider + charger on this net (`Power_schematic.jpg`) |
| 6 | P2/P3 — DS1302 **CE candidate** | reserved (h02) | **R1 open**: P3/P4 clearly expose only IO17/IO18; IO6/IO7 are shared with P2. Awaiting board designer |
| 7 | P2/P3 — spare (DS1302 CE fallback; harness col) | free (claimed by devkit) | shares P3/P4 muxing with IO6 |
| 8 | Touch I2C SCL | **used** | `I2C0_SCL_GPIO8` (`overlay:89-91`) |
| 9 | P2 | **do not use** | BOOT strapping — reserved for the deliberate hold-at-reset bootloader combo only (R8) |
| 10–13 | microSD SPI (CS/CLK/MOSI/MISO = 10/12/11/13) | **reserved** | SD decision open (R3); pins stay out of any allocation |
| 14 | harness row / spare | free | moved out of phantom-kscan service in h01 (was the phantom row) |
| 15 | harness row / spare | free | was I2S LRCLK conflict zone; now clean |
| 16 | harness col / spare | free | |
| 17 | P3 — DS1302 SCLK (proposed) | reserved (h02) | clearly exposed on P3/P4 |
| 18 | P3 — DS1302 IO (proposed) | reserved (h02) | clearly exposed on P3/P4 |
| 19/20 | USB D-/D+ | **used** | never repurpose **in firmware**; while the app runs the USB device stack drives them as OTG (reverts at reset — no eFuse; see `tab1a-bench-runbook.md` §b06) |
| 21 | QSPI D0 | **used** | `overlay:46` |
| 38 | Panel TE | spare | routed, currently unused by the driver |
| 39/40 | QSPI D2/D3 | **used** | `overlay:64-67` |
| 41 | I2S0 DATA out (`I2S0_O_SD_GPIO41`) → NS4168 DIN | reserved (h03) | speaker pads P5/±VAT→-SP downstream |
| 42 | I2S0 BCLK (`I2S0_O_BCK_GPIO42`) | reserved (h03) | |
| 43/44 | Console UART0 TX/RX | **used** | don't key-scan. **tab-01 bench note (2026-11-28):** `RXD2` verified end-to-end (shell commands via P1 reboot the tab); TXD-side bytes never reached the bench, but a meter confirms GPIO43→P1-TXD is continuous and the pinctrl driver provably drives the pad (same apply pass as the working RX cell) — the prime suspect is the bench jumper itself (back-pushed pin after a swap), not the board. `bringup/tab1a-console-ioport.overlay` (TX→IO17) stands as fallback if the wire check clears |
| 45 | QSPI CS (via `&gpio1 13`) | **used** | also VDD_SPI strapping — keep as-is |
| 46 | WS2812 light-bar DIN (h04/devkit) | reserved (h04) | **strapping pin** (ROM message output): a WS2812 DIN idles the line low at reset → ROM log behaviour changes but boot is unaffected; document on the final board |
| 47 | QSPI CLK | **used** | `overlay:43` |
| 48 | QSPI D1 | **used** | `overlay:61`. Also JTX/JTAG-select strapping — already committed to the panel |
| 0, 45 | (S3 strapping set: 0, 3, 45, 46) | | 0 = BOOT (breakout button), 45 = VDD_SPI |

GPIO numbering note: the S3 port splits controllers — `&gpio0` = IO0–31,
`&gpio1` = IO32–48 (IO46 → `&gpio1 14`).

## GDMA channels

| Ch | Owner | Source |
|---|---|---|
| 0/1 | `spi2` (panel MSPI) | `overlay:145-146` |
| 2/3 | `i2s0` (default; h03/devkit) | `zephyr/dts/xtensa/espressif/esp32s3/esp32s3_common.dtsi:326-341` |
| 4/5 | `i2s1` (free) | same |
| 6/7 | `lcd_cam` (unused) | comment `overlay:140-141` |

## Peripheral wiring decisions

- **DS1302 RTC (h02)** — proposed SCLK=IO17, IO=IO18, **CE=IO6 (TBD, R1)**.
  The in-tree Zephyr driver (`zephyr/drivers/rtc/rtc_ds1302.c`) is SPI-API
  based (half-duplex, LSB) and there is **no GPIO bit-bang SPI driver in this
  tree** (`drivers/spi/` has no `spi_gpio`) → h02 ports the pad10a GPIO
  bit-bang as a Zephyr `rtc` driver instead; no `spi3` node is added until
  that decision lands.
- **Speaker (h03)** — on-board NS4168-class amp on the SOUND pads:
  LRCLK 2 / BCLK 42 / DATA 41, i2s0 defaults (GDMA 2/3). No ZMK I2S code
  exists yet; h03 adds TX + volume. Amp auto-enables from I2S clock (no EN pin
  observed — verify on the final-board schematic review).
- **Keys + light bar (h04)** — 4 keys on P2, matrix 2×2 **rows IO7/IO14,
  cols IO15/IO16** (excludes IO5=VBAT, IO9=BOOT — R8/R2; diodes row→col).
  Earlier text had rows IO14/15 × cols IO16/7 — wrong; the h01 harness and
  the product `cobantab1a-layout.dtsi` both run rows 7/14 × cols 15/16,
  bench-verified. Button→position order is set by the transform `RC()`
  list, not the GPIO choice. WS2812 bar DIN IO46.
- **VBAT (p01)** — `adc1` ch 4 (`ADC1_GPIO5_CHANNEL = 4`,
  `modules/hal/espressif/components/soc/esp32s3/include/soc/adc_channel.h:21`);
  divider ratio **measure with a bench meter** and record here (see runbook).
- **SD (open)** — IO10–13 stay reserved per R3; asset store keeps a 2nd
  backend open.

## Known conflicts ledger (the traps)

1. **IO5** is VBAT ADC — never a key (R8). Also the *old* phantom-kscan col;
   relocated by h01 (now rows IO14 / cols IO15 — see
   `cobantab1a-layout.dtsi`).
2. **IO2** is I2S LRCLK — the *old* phantom-kscan row; same relocation.
3. **IO9** BOOT strapping — only the hold-at-reset bootloader combo (R8).
4. **P3 third signal** (DS1302 CE): P3/P4 expose IO17/IO18 only; IO6/IO7 are
   shared with P2 — **open (R1)**, board designer asked 2026-11-21.
5. **IO46** is a strapping pin; acceptable as WS2812 DIN — but concretely:
   the DIN idles low at reset, and GPIO46-low **disables ROM message output
   entirely** on the S3, so **no ROM boot banner ever appears on this board**
   (bench-confirmed 2026-11-28 — it made every P1 silence look like a wiring
   fault). No effect on boot itself; record on final board.
6. IO48 is both QSPI D1 and JTAG-select — JTAG debugging trades the panel.
7. No back-powering through header pins (matrix-fixture rule,
   `fixtures/matrix-fixture.md:100-108`).

## Dev harness (`tab1a-devkit` build)

Overlay/conf: `zmk/esp-port/bringup/tab1a-devkit.{overlay,conf}` — usage at the
top of the overlay. Bench wiring checklist + capture recipe:
[`tab1a-bench-runbook.md`](tab1a-bench-runbook.md).

| Harness part | Pins | Build symbol / probe |
|---|---|---|
| 4 keys (2×2 matrix) | rows IO7/IO14, cols IO15/IO16 (to GND via diodes row→col) | kscan; probe logs positions |
| WS2812 chase | DIN IO46 (`&gpio1 14`) **via SPI3**, SCLK IO38 (devkit-only, spare TE), 5V/GND common with board | `worldsemi,ws2812-spi` + probe chase (GPIO bit-bang strip driver is nRF-only in this Zephyr) |
| I2S tone | IO2/42/41 → SOUND pads, speaker on P5 | i2s0 + probe 1 kHz tone |
| VBAT | header VBAT pad; meter across divider | probe logs `adc1` ch4 raw |
| DS1302 | P3 (SCLK 17 / IO 18 / CE 6-TBD) | **no node yet** — h02 driver decision (see above) |

## Compat-matrix row

Added to [`compat-matrix.md`](compat-matrix.md) (row "Tab 1a proto").
