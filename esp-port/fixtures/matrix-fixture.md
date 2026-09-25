# Direct-GPIO matrix keyboard fixture

Phase 0 deliverable: a reproducible physical keyboard fixture for flashing,
matrix-scan validation, and BLE/USB typing tests on the ESP32-C3 and
ESP32-S3 SuperMini reference boards.

Design goals (per `implement_plan.md` Phase 0 task 3):

- Direct GPIO matrix (no MCU, no kscan hardware).
- No conflict with flash, boot strapping, UART/JTAG, onboard LED, or native
  USB pins.
- Identical physical fixture for both chips; only the pin map changes.

## BOM

| Qty | Part | Notes |
| --- | --- | --- |
| 16 | 6 × 6 mm tactile switches (4 × 4) | any standard kbd switch |
| 16 | 1N4148 (or 1N4148W) diodes | one per switch, anode → column |
| 8 | 10 kΩ resistors | row pull-ups (3.3 V) |
| 1 | 4 × 14 perfboard or breadboard | |
| 1 | JST-SH / pin header breakout per chip | matches the SuperMini pin headers |
| — | 3.3 V supply or USB power to the board | board is powered from USB-C |

Wiring: each switch sits between a row line and a column line; the diode is in
series on the column side (anode at the switch, cathode to the column net).
Row nets are pulled up to 3.3 V through 10 kΩ and driven low to scan. This is
the same topology ZMK's `zmk,gpio-keys`/matrix drivers expect; record the
diode direction here and keep it identical for both chips so keymap files can
be shared.

> **TODO (hardware):** photograph the finished fixture (top + bottom), mark
> the row/column nets on the photo, and link them in
> `reference-boards/`.

## Pin maps

### ESP32-C3 SuperMini (target marking ESP32-C3FN4, 4 MB flash)

Rows: GPIO0, GPIO1, GPIO3, GPIO4. Columns: GPIO5, GPIO10, GPIO20, GPIO21.

| Net | GPIO | Notes |
| --- | --- | --- |
| ROW0 | GPIO0 | 32 kHz crystal pad (XTAL_32K_P); free when no crystal is fitted — the common SuperMini revision has none, verify at intake |
| ROW1 | GPIO1 | 32 kHz crystal pad (XTAL_32K_N); free when no crystal is fitted |
| ROW2 | GPIO3 | |
| ROW3 | GPIO4 | external JTAG MTMS — usable as GPIO; disables the external JTAG path only, USB Serial/JTAG is unaffected |
| COL0 | GPIO5 | external JTAG MTDI — usable as GPIO; USB Serial/JTAG unaffected |
| COL1 | GPIO10 | |
| COL2 | GPIO20 | UART0 RX default (U0RXD) — console moves to USB Serial/JTAG |
| COL3 | GPIO21 | UART0 TX default (U0TXD) — console moves to USB Serial/JTAG |

Reserved (do **not** use):

- GPIO18, GPIO19 — USB Serial/JTAG D-/D+ (USB-C). Losing these loses the
  flash/console/recovery path.
- GPIO8 — onboard active-low LED (also strapping).
- GPIO9 — BOOT button (strapping; low = download mode).
- GPIO2 — strapping pin (floating at reset; no boot-mode function on the
  C3 — download mode is selected by GPIO9). Keep free.
- GPIO12–GPIO17 — in-package flash SPI (not exposed on the board).

Consequence: with COL2/COL3 on GPIO20/21 the UART0 console is unavailable;
flashing, logging, and recovery all use USB Serial/JTAG. This is intended.

### ESP32-S3 SuperMini (target marking ESP32-S3FH4R2, 4 MB flash + 2 MB QSPI PSRAM)

Rows: GPIO1, GPIO2, GPIO4, GPIO5. Columns: GPIO6, GPIO7, GPIO8, GPIO15.

| Net | GPIO | Notes |
| --- | --- | --- |
| ROW0 | GPIO1 | |
| ROW1 | GPIO2 | |
| ROW2 | GPIO4 | |
| ROW3 | GPIO5 | |
| COL0 | GPIO6 | |
| COL1 | GPIO7 | |
| COL2 | GPIO8 | |
| COL3 | GPIO15 | |

Reserved (do **not** use):

- GPIO0 — strapping (download mode; BOOT button).
- GPIO3 — strapping (JTAG signal source).
- GPIO19, GPIO20 — USB-C D-/D+ (USB Serial/JTAG by default; see Phase 8 for
  the OTG/eFuse question).
- GPIO45 — strapping (VDD_SPI voltage select).
- GPIO46 — strapping (boot mode / ROM-message control).
- GPIO48 — JTAG-source-select pin; also the WS2812 RGB + red onboard-LED
  net.
- GPIO26–GPIO32 — in-package flash + QSPI PSRAM (not exposed on the board).
  On the FH4R2 quad config **GPIO33–38 are free** (the DQ4–DQ7/DQS pins are
  octal-only) — back-side castellated pads.
- GPIO43, GPIO44 — UART0 TX/RX; **kept free as the recovery console** until
  USB behavior is qualified (Phase 8).

Additional S3 pins available for later fixture expansion (not in the default
map): GPIO16, GPIO17, GPIO18, GPIO21, and GPIO33–38 (back-side pads).

## Conflict notes

- No matrix pin is a strapping pin on either chip (C3 strapping =
  GPIO2/8/9, S3 strapping = GPIO0/3/45/46 — none appear in the maps). The
  10 kΩ row pull-ups still give a clean high reset state.
- No matrix pin touches the flash/PSRAM SPI or USB nets on either chip. On
  the C3, ROW3 (GPIO4) and COL0 (GPIO5) are the external JTAG pins
  (MTMS/MTDI); driving them as GPIO disables the external JTAG path only —
  USB Serial/JTAG (the flash/console/recovery path) is unaffected.
- The fixture must not power any board rail other than through the board's
  own USB-C input; do not back-power GPIOs from an external supply.

## Keymap mapping (for test keymaps)

4 × 4 matrix, row-major, diode direction as above:

```
ROW0: K(1)  K(2)  K(3)  K(4)
ROW1: K(5)  K(6)  K(7)  K(8)
ROW2: K(9)  K(0)  K(MINUS) K(EQUAL)
ROW3: K(A)  K(B)  K(C)  K(D)
```

A test `.keymap` using this matrix is added in Phase 5/6 together with the
board fixture shield; the physical positions above are the reference for
press/verify steps in `test-results/`.
