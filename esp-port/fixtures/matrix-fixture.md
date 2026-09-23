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

### ESP32-C3 SuperMini (C3FX4/FN4, 4 MB)

Rows: GPIO0, GPIO1, GPIO3, GPIO4. Columns: GPIO5, GPIO10, GPIO20, GPIO21.

| Net | GPIO | Notes |
| --- | --- | --- |
| ROW0 | GPIO0 | strapping (low = download mode) — pulled up, driven low only during scan; acceptable, verify boot |
| ROW1 | GPIO1 | |
| ROW2 | GPIO3 | |
| ROW3 | GPIO4 | |
| COL0 | GPIO5 | |
| COL1 | GPIO10 | |
| COL2 | GPIO20 | UART0 TX default — console moves to USB Serial/JTAG |
| COL3 | GPIO21 | UART0 RX default — console moves to USB Serial/JTAG |

Reserved (do **not** use):

- GPIO18, GPIO19 — USB Serial/JTAG D-/D+ (USB-C). Losing these loses the
  flash/console/recovery path.
- GPIO8 — onboard active-low LED (also strapping).
- GPIO9 — BOOT button (strapping; low = download mode).
- GPIO2 — strapping (low = download mode).

Consequence: with COL2/COL3 on GPIO20/21 the UART0 console is unavailable;
flashing, logging, and recovery all use USB Serial/JTAG. This is intended.

### ESP32-S3 SuperMini (target marking ESP32-S3FH4R2, 4 MB flash + 2 MB QSPI PSRAM)

Rows: GPIO1, GPIO2, GPIO4, GPIO5. Columns: GPIO6, GPIO7, GPIO8, GPIO15.

| Net | GPIO | Notes |
| --- | --- | --- |
| ROW0 | GPIO1 | |
| ROW1 | GPIO2 | strapping (low = download mode) — pulled up, driven low only during scan; verify boot |
| ROW2 | GPIO4 | |
| ROW3 | GPIO5 | |
| COL0 | GPIO6 | |
| COL1 | GPIO7 | |
| COL2 | GPIO8 | |
| COL3 | GPIO15 | |

Reserved (do **not** use):

- GPIO0, GPIO3 — strapping (download / boot mode).
- GPIO19, GPIO20 — USB-C D-/D+ (USB Serial/JTAG by default; see Phase 8 for
  the OTG/eFuse question).
- GPIO45, GPIO46 — strapping (VDD_SPI voltage, JTAG source).
- GPIO48 — JTAG source strapping.
- GPIO33–GPIO38 — SPI0/1 flash + QSPI PSRAM; never usable.
- GPIO43, GPIO44 — UART0 TX/RX; **kept free as the recovery console** until
  USB behavior is qualified (Phase 8).

Additional S3 pins available for later fixture expansion (not in the default
map): GPIO16, GPIO17, GPIO18, GPIO21.

## Conflict notes

- ROW0 on C3 (GPIO0) and ROW1 on S3 (GPIO2) are download-mode strapping pins.
  The 10 kΩ pull-up keeps them high at reset; the scanner drives them low only
  briefly during a scan cycle. Verify on first flash that the board still
  boots normally; if it doesn't, move that row to a non-strapping pin
  (C3: GPIO10; S3: GPIO16) and update the map.
- No matrix pin touches the flash/PSRAM SPI, JTAG, or USB nets on either chip.
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
