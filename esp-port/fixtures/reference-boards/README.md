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
- Target marking: <e.g. ESP32-C3FX4 / ESP32-S3FH4R2>
- Actual chip marking: <photo + transcription>
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

## Status

| Board | File | Status |
| --- | --- | --- |
| C3 #1 | — | pending acquisition |
| C3 #2 | — | pending acquisition |
| S3 #1 | — | pending acquisition |
| S3 #2 | — | pending acquisition |
