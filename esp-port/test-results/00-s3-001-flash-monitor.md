# 00-001 — flash-monitor (ESP32-S3 SuperMini)

- Date: 2026-09-26
- Test case: flash-monitor
- Result: PASS
- Duration: not recorded (build ~2m, flash ~1s, monitor verified live)

## Environment

- Board: S3 SuperMini generic **unit 01**
  (`fixtures/reference-boards/esp32s3-supermini-generic-01.md`), MAC
  `90:da:72:75:9b:84` (unit confirmed by owner 2026-09-26)
- Board/module revision: TBD (intake)
- Chip marking: reported `FH4R2P…`; esptool reports ESP32-S3 (QFN56)
  **revision v0.2** (tracking 2nd char C per reference-boards README)
- Flash / PSRAM: 4 MB (XMC) + 2 MB PSRAM (AP_3v3) (esptool)
- USB-PHY / eFuse state: esptool reports **USB mode USB-Serial/JTAG**
  (native, no external PHY); eFuse `USB_PHY_SEL` not yet dumped (follow-up)
- Firmware: RyanDam/zmk @ `212467661804ea9b3f60a88866a2caf4f3e87997`
  (branch `feat/esp32`), target `esp32s3_devkitc/esp32s3/procpu`, sample
  `zephyr/samples/philosophers` + `esp-port/fixtures/usb-console.overlay`,
  image SHA-256
  `a1b9c5a4f52ab9aba7966d538252cbf005303c4c50bb3ccb96356659a23eb847`
  (135900 B)
- Zephyr: `58a5874a446ace2893a196848282d271a551e512`
  (`v4.1.0+zmk-fixes`); hal_espressif:
  `202c59552dc98e5cd02386313e1977ecb17a131f`
- Toolchain: Espressif xtensa-esp32s3-elf 12.2.0_20230208 (GCC 12.2.0),
  west v1.5.0, CMake 3.31.6; host flasher esptool v5.4.0
- Host OS: build in Linux container (aarch64); flash/monitor on macOS,
  port `/dev/tty.usbmodem834401` (cu: `/dev/cu.usbmodem834401`)
- Power: USB 5 V via host

## Procedure

1. Container: `sh esp-port/build_esp32.sh --board s3 --sample philosophers`
   → `build/s3-philosophers` (FLASH 135900 B / 1.62%).
2. macOS host: `cd /Users/ryan/Documents/CobanStationeryAssets/Firmware/src/zmk`
   then
   `esptool --port /dev/tty.usbmodem834401 write-flash 0x0 build/s3-philosophers/zephyr/zephyr.bin`.
3. macOS host: `screen /dev/cu.usbmodem834401 115200`.

## Observations / measurements

- esptool: "Connected to ESP32-S3 on /dev/tty.usbmodem834401" — chip
  ESP32-S3 (QFN56) rev v0.2, Wi-Fi + BT 5 (LE), dual core + LP core
  240 MHz, 4 MB XMC flash + 2 MB embedded PSRAM (AP_3v3), 40 MHz crystal,
  USB-Serial/JTAG, MAC `90:da:72:75:9b:84`.
- Wrote 135900 bytes (41113 compressed) at 0x0 in 0.6 s; "Hash of data
  verified"; "Hard resetting via RTS pin".
- Monitor shows continuous philosophers output (console on USB Serial/JTAG
  via `usb-console.overlay`) — the run that proved the monitor path.
- Measured current: n/a

## Artifacts

- logs: `logs/00-s3-001-flash-monitor.log` (working copy: `tmp/esps3_log.log`,
  gitignored)
- photos: n/a

## Notes / follow-ups

- Dump eFuse `USB_PHY_SEL` read-only (`esptool efuse dump`) at intake —
  the native USB-Serial/JTAG mode observed here is consistent with an
  unburned PHY select, but the eFuse state itself is not yet recorded.
- Satisfies the Phase 0 exit gate "at least one developer can flash/monitor
  each ESP development board independently" for the S3.
