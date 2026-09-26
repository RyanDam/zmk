# 00-001 — flash-monitor (ESP32-C3 SuperMini)

- Date: 2026-09-26
- Test case: flash-monitor
- Result: PASS
- Duration: not recorded (build ~2m, flash ~1s, monitor verified live)

## Environment

- Board: C3 SuperMini generic **unit 01**
  (`fixtures/reference-boards/esp32c3-supermini-generic-01.md`), MAC
  `ec:da:3b:bf:6a:30` (unit confirmed by owner 2026-09-26)
- Board/module revision: TBD (intake)
- Chip marking: reported `FN4P…`; esptool reports ESP32-C3 (QFN32)
  **revision v0.4** (tracking 2nd char E per reference-boards README)
- Flash / PSRAM: 4 MB (XMC) / none (esptool)
- USB-PHY / eFuse state: eFuse n/a (C3); esptool reports **USB mode
  USB-Serial/JTAG** (native, no external PHY)
- Firmware: RyanDam/zmk @ `212467661804ea9b3f60a88866a2caf4f3e87997`
  (branch `feat/esp32`), target `esp32c3_devkitc/esp32c3`, sample
  `zephyr/samples/philosophers` + `esp-port/fixtures/usb-console.overlay`,
  image SHA-256
  `0374ebc2f7cccf1526636b0be8f0b671c18fe406ce36de42294877027533ff44`
  (133060 B)
- Zephyr: `58a5874a446ace2893a196848282d271a551e512`
  (`v4.1.0+zmk-fixes`); hal_espressif:
  `202c59552dc98e5cd02386313e1977ecb17a131f`
- Toolchain: Espressif riscv32-esp-elf 12.2.0_20230208 (GCC 12.2.0),
  west v1.5.0, CMake 3.31.6; host flasher esptool v5.4.0
- Host OS: build in Linux container (aarch64); flash/monitor on macOS,
  port `/dev/tty.usbmodem834401` (cu: `/dev/cu.usbmodem834401`)
- Power: USB 5 V via host

## Procedure

1. Container: `sh esp-port/build_esp32.sh --board c3 --sample philosophers`
   → `build/c3-philosophers` (FLASH 133060 B / 3.17%).
2. macOS host: `cd /Users/ryan/Documents/CobanStationeryAssets/Firmware/src/zmk`
   then
   `esptool --port /dev/tty.usbmodem834401 write-flash 0x0 build/c3-philosophers/zephyr/zephyr.bin`.
3. macOS host: `screen /dev/cu.usbmodem834401 115200`.

## Observations / measurements

- esptool: "Connected to ESP32-C3 on /dev/tty.usbmodem834401" — chip
  ESP32-C3 (QFN32) rev v0.4, Wi-Fi + BT 5 (LE), single core 160 MHz,
  4 MB XMC flash, 40 MHz crystal, USB-Serial/JTAG, MAC
  `ec:da:3b:bf:6a:30`.
- Wrote 133060 bytes (34219 compressed) at 0x0 in 0.8 s; "Hash of data
  verified"; "Hard resetting via RTS pin".
- Monitor shows continuous philosophers output (console on USB Serial/JTAG
  via `usb-console.overlay`) — the run that proved the monitor path.
- Measured current: n/a

## Artifacts

- logs: `logs/00-c3-001-flash-monitor.log` (working copy: `tmp/espc3_log.log`,
  gitignored)
- photos: n/a

## Notes / follow-ups

- Record the USB VID/PID (`lsusb` on the host) at intake — esptool does not
  print it.
- Satisfies the Phase 0 exit gate "at least one developer can flash/monitor
  each ESP development board independently" for the C3.
