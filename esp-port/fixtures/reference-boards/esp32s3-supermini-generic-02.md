# ESP32-S3 SuperMini — generic unit 02

- Acquired: 2026-09 (record exact date at intake)
- Vendor / lot / serial: TBD — record at intake; rename file if a specific
  vendor or lot is identified
- Target marking: ESP32-S3FH4R2 (4 MB flash + 2 MB QSPI PSRAM, QFN56)
- Actual chip marking: reported `ESP32-S3` / `FH4R2P…` — full transcription
  (incl. complete tracking code) + photo pending intake
- PCB revision: TBD (silkscreen marking, photo)
- Flash: 4 MB in-package (per target marking)
- PSRAM: 2 MB quad-SPI (per target marking; disabled by default in
  toolchains — enable as *quad*, never octal)

Role: **spare / sacrificial** (unit 01 is the qualified reference). This is
the unit reserved for Phase 8 eFuse / destructive experiments (e.g. burning
`USB_PHY_SEL`).

## Photos

- top: `photos/top.jpg` — TODO
- bottom: `photos/bottom.jpg` — TODO
- chip: `photos/chip.jpg` — TODO
- usb-c: `photos/usb-c.jpg` — TODO

## Wiring as found

Expected values from the common-revision documentation (see `README.md` →
Known board information). Fill the Evidence column at intake (continuity
with the board unpowered, or boot log).

| Signal | Net / GPIO | Evidence (continuity / schematic / boot log) |
| --- | --- | --- |
| USB-C D- | GPIO19 (expected) | TODO |
| USB-C D+ | GPIO20 (expected) | TODO |
| Onboard LED | GPIO48: WS2812 RGB **and** red LED share the net (expected); charge LED separate | TODO |
| BOOT button | GPIO0 (expected) | TODO |
| RESET button | chip EN (expected) | TODO |
| Battery pads (if present) | B+/B− back pads + on-board Li charger; BOOST jumper 100 mA (unbridged) / 300 mA (bridged) | TODO |

## Intake checklist

- [ ] Photograph top / bottom / chip / USB-C; save to `photos/`
- [ ] Transcribe full chip marking (product name, `FH4R2`, date code,
      complete tracking code) → silicon revision per `README.md`
- [ ] Transcribe PCB revision / silkscreen
- [ ] Verify chip marking is `FH4R2` (4 MB + 2 MB QSPI PSRAM). Mismatch →
      record in `../compat-matrix.md` and reorder; do not silently
      substitute
- [ ] **Antenna orientation check**: feed trace must run to the antenna
      pad marked with the white bar; some units ship backwards (element
      not connected) — visual + note
- [ ] Check for an external USB PHY chip near the USB-C (small QFN) →
      record yes/no (Phase 8 gate)
- [ ] USB-C data check: host sees a new USB CDC device on plug
- [ ] Continuity: USB-C D- ↔ GPIO19, D+ ↔ GPIO20 (board unpowered)
- [ ] Map LED / BOOT / RESET / battery nets against silkscreen
- [ ] eFuse read (read-only, after first flash): `esptool.py --port <port>
      efuse dump` → record `USB_PHY_SEL` = unburned/burned. **Never write
      eFuses on a dev board.**
- [ ] Fill the Evidence column above; note any deviation below

## Deviations from the target reference

- none recorded yet

## Notes

- PSRAM: confirm 2 MB is detected once a build enables QSPI PSRAM
  (`ESP.getPsramSize()` in Arduino, or `CONFIG_SPIRAM` in Zephyr). If it
  reports 0, check the chip marking and PSRAM mode before assuming a
  defect.
- GPIO48 drives both the WS2812 and the red LED; it is also the
  JTAG-source-select pin. Do not use it for the matrix fixture.
- Antenna: some units ship with it soldered backwards — see intake
  checklist.
- Phase 8 eFuse burning (e.g. `USB_PHY_SEL`) happens on **this** unit, not
  on unit 01. Burning requires explicit approval per the compat-matrix
  conventions.
- 2026-09-26: unit 01 passed the flash/monitor bring-up (MAC
  `90:da:72:75:9b:84`); this unit (02) not yet tested — see
  `../../test-results/00-s3-001-flash-monitor.md`.
