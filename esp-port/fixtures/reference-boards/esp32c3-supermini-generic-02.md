# ESP32-C3 SuperMini — generic unit 02

- Acquired: 2026-09 (record exact date at intake)
- Vendor / lot / serial: TBD — record at intake; rename file if a specific
  vendor or lot is identified
- Target marking: ESP32-C3FN4 (4 MB flash, no PSRAM, QFN32)
- Actual chip marking: reported `ESP32-C3` / `FN4P…` — full transcription
  (incl. complete tracking code) + photo pending intake
- PCB revision: TBD (silkscreen marking, photo)
- Flash: 4 MB in-package (per target marking)
- PSRAM: none (C3 has no PSRAM)

Role: **spare / sacrificial** (unit 01 is the qualified reference). This is
the unit reserved for Phase 8 eFuse / destructive experiments.

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
| USB-C D- | GPIO18 (expected) | TODO |
| USB-C D+ | GPIO19 (expected) | TODO |
| Onboard LED | GPIO8, active-low blue (expected); red power LED not GPIO | TODO |
| BOOT button | GPIO9 (expected) | TODO |
| RESET button | chip EN (expected) | TODO |
| Battery pads (if present) | none expected on C3 SuperMini — verify visually | TODO |

## Intake checklist

- [ ] Photograph top / bottom / chip / USB-C; save to `photos/`
- [ ] Transcribe full chip marking (product name, `FN4`, date code,
      complete tracking code) → silicon revision per `README.md`
- [ ] Transcribe PCB revision / silkscreen
- [ ] Verify chip marking is `FN4` (4 MB). Mismatch → record in
      `../compat-matrix.md` and reorder; do not silently substitute
- [ ] Verify no 32 kHz crystal fitted (GPIO0/1 free) — visual
- [ ] USB-C data check: host sees a new USB CDC device on plug
- [ ] Continuity: USB-C D- ↔ GPIO18, D+ ↔ GPIO19 (board unpowered)
- [ ] Map LED / BOOT / RESET nets against silkscreen
- [ ] Fill the Evidence column above; note any deviation below

## Deviations from the target reference

- none recorded yet

## Notes

- C3FN4 is NRND (successor C3FH4, only difference is the temperature
  range) — recorded for the compatibility matrix, no action needed.
- Antenna: CrossAir CA-C03 ceramic, known-weak Wi-Fi range (layout issue).
  Note observed range quality here if it matters for a test.
- Do not burn eFuses or run destructive tests on unit 01; use this unit.
