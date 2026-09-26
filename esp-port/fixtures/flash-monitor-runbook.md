# Flash / monitor / recovery runbook (ESP32-C3 & ESP32-S3 SuperMini)

Phase 0 exit gate: *at least one developer can flash/monitor each ESP
development board independently.* This runbook is the reference procedure;
each step below must be executed once per board and the result recorded in
`test-results/` (test case `flash-monitor`).

## Common prerequisites

- Zephyr SDK installed (baseline: 0.16.9) and `west` on PATH.
- **Espressif ESP32 toolchain installed** (not in the ZMK ARM dev image):
  `riscv32-esp-elf` + `xtensa-esp32s3-elf` 12.2.0 under
  `/opt/espressif/tools`. Install + rationale: `../toolchain-esp32.md`.
  Every ESP32 build needs these two exported **for that build only** (do not
  set them globally — ARM ZMK boards still use the Zephyr SDK ARM toolchain):

  ```sh
  export ZEPHYR_TOOLCHAIN_VARIANT=espressif
  export ESPRESSIF_TOOLCHAIN_PATH=/opt/espressif/tools
  ```

  Easiest: `sh esp-port/build_esp32.sh [--board c3|s3|all] [--sample
  hello|philosophers] [--flash]` sets these for you, does a clean build, and
  applies the USB console overlay (see `../toolchain-esp32.md`). The per-board
  commands below show the manual equivalent.

- A USB-C **data** cable (many SuperMini bundles ship charge-only cables —
  verify by checking that the host sees a new serial/USB device on plug).
- Workspace initialized: `west init -l app && west update && west zephyr-export`
  (or an existing west workspace).
- 3.3 V supply + current meter available for later power tests (Phase 7);
  not needed for flash/monitor.
- Run the build/flash/monitor commands from the **workspace root** (the
  directory containing `.west`, e.g. `/workspaces/zmk`). The sample path is
  `zephyr/samples/hello_world` — `west build` resolves it relative to your
  current directory, so `samples/hello_world` from `app/` fails.

## ESP32-C3 SuperMini

Flash path: USB Serial/JTAG on GPIO18/19 (USB-C). **Console:** the devkitc
board definition routes the Zephyr console to UART0 (GPIO21 TX), which the
SuperMini does not bridge to USB — so the build must apply
`esp-port/fixtures/usb-console.overlay`, which routes the console to the USB
Serial/JTAG port (the same port used for flashing). Without it, `west monitor`
on the USB port shows nothing.

```sh
export ZEPHYR_TOOLCHAIN_VARIANT=espressif
export ESPRESSIF_TOOLCHAIN_PATH=/opt/espressif/tools

# 1. Build a minimal sample (first-time bring-up).
#    Note: the esp32c3_supermini board only exists from Zephyr 4.4 (Phase 5+).
#    On the current 4.1 baseline use the pin-compatible devkitc board.
#    The overlay routes the console to USB Serial/JTAG (see above); the path
#    must be absolute.
west build -b esp32c3_devkitc/esp32c3 -d build/c3-hello zephyr/samples/hello_world \
  -- -DEXTRA_DTC_OVERLAY_FILE=/workspaces/zmk/esp-port/fixtures/usb-console.overlay

# 2. Flash — two equivalent paths:
#    a) board attached to the build host (esptool via the Zephyr runner;
#       board is held in download mode automatically by the USB Serial/JTAG
#       controller on reset):
west flash -d build/c3-hello
#    b) board attached to another machine (VERIFIED 2026-09-26, macOS host):
esptool --port /dev/tty.usbmodem834401 write-flash 0x0 build/c3-philosophers/zephyr/zephyr.bin

# 3. Monitor — two equivalent paths:
west monitor -d build/c3-hello
# or, on the host machine (VERIFIED 2026-09-26):
screen /dev/cu.usbmodem834401 115200
```

Expected (observed 2026-09-26, record in
`../test-results/00-c3-001-flash-monitor.md`): USB CDC serial device on plug
(`/dev/cu.usbmodem*` on macOS); esptool reports ESP32-C3 (QFN32) rev v0.4,
4 MB XMC flash, 40 MHz crystal, **USB mode USB-Serial/JTAG**; monitor shows
continuous philosophers output at 115200 (console on USB Serial/JTAG via the
overlay).

**hello_world prints once, at boot.** To verify the monitor without catching
the boot moment, build the continuously-printing sample instead
(`zephyr/samples/philosophers`, or `sh esp-port/build_esp32.sh --board c3
--sample philosophers --flash`). The wrapper uses a per-sample build dir, so
monitor `build/c3-philosophers` (not `build/c3-hello`) in that case.

**Reset kills the monitor:** pressing RESET re-enumerates the chip's USB
Serial/JTAG interface, so the ttyACM device disappears and the monitor exits.
Re-attach the monitor; with the philosophers sample the output resumes
immediately, so no boot moment needs to be caught. (A UART0 + USB-UART bridge
connection survives resets — see the S3 section.)

Download-mode recovery (if the board bricked): hold **BOOT** (GPIO9) while
pulsing **RESET**, release BOOT; the ROM loader takes over on USB-C.

> **TODO (hardware):** record the USB VID/PID (`lsusb` on the host — esptool
> does not print it); confirm BOOT/RESET silkscreen positions in
> `reference-boards/`. (Resolved 2026-09-26: a data cable works — the host
> sees the USB CDC device on plug, and flash + monitor succeeded over it.)

## ESP32-S3 SuperMini

Flash path: USB Serial/JTAG on GPIO19/20 (USB-C). **Console:** same as the C3 —
the board default is UART0 (GPIO43 TX), not bridged to USB on the SuperMini.
Note the S3 devkitc board definition *explicitly disables* the `usb_serial`
node, so the `usb-console.overlay` (which re-enables it) is mandatory here.
**Recovery console:** UART0 on GPIO43 (TX) / GPIO44 (RX) — keep these two pins
free of the matrix fixture.

```sh
export ZEPHYR_TOOLCHAIN_VARIANT=espressif
export ESPRESSIF_TOOLCHAIN_PATH=/opt/espressif/tools

# 1. Build (no upstream S3 SuperMini board exists yet — for Phase 0 bring-up
#    use the closest upstream board, e.g. esp32s3_devkitc; the dedicated
#    esp32s3_supermini board lands in Phase 6). The S3 is dual-core, so a
#    core qualifier is required: procpu = primary core 0 (appcpu = core 1).
west build -b esp32s3_devkitc/esp32s3/procpu -d build/s3-hello zephyr/samples/hello_world \
  -- -DEXTRA_DTC_OVERLAY_FILE=/workspaces/zmk/esp-port/fixtures/usb-console.overlay

# 2. Flash — two equivalent paths:
#    a) board attached to the build host:
west flash -d build/s3-hello
#    b) board attached to another machine (VERIFIED 2026-09-26, macOS host):
esptool --port /dev/tty.usbmodem834401 write-flash 0x0 build/s3-philosophers/zephyr/zephyr.bin

# 3. Monitor — two equivalent paths:
west monitor -d build/s3-hello
# or, on the host machine (VERIFIED 2026-09-26):
screen /dev/cu.usbmodem834401 115200
```

Expected (observed 2026-09-26, record in
`../test-results/00-s3-001-flash-monitor.md`): USB CDC serial device on plug;
esptool reports ESP32-S3 (QFN56) rev v0.2, dual core + LP core 240 MHz,
4 MB XMC flash + 2 MB PSRAM (AP_3v3), 40 MHz crystal, **USB mode
USB-Serial/JTAG**; monitor shows continuous philosophers output at 115200
(plus the same hello_world/philosophers and reset-re-attach notes as the C3).

Download-mode recovery: hold **BOOT** (GPIO0) while pulsing **RESET**.

UART0 recovery (last resort, e.g. after a bad USB-related firmware in Phase 8):
wire GPIO43 (TX) → RXI and GPIO44 (RX) → TXI of a 3.3 V UART bridge (USB-UART,
115200 8N1, no flow control); the ROM loader and Zephyr console both fall back
to UART0 when USB Serial/JTAG is not responding.

> **TODO (hardware):** continuity-test the USB-C ↔ GPIO19/20 route on the
> reference sample, and record eFuse `USB_PHY_SEL` state (read-only:
> `esptool.py efuse dump` or ESP-IDF `efuse_summary.py`). Never write eFuses
> on a development board. (Resolved 2026-09-26: esptool reports "USB mode:
> USB-Serial/JTAG" — the native internal PHY is in use, no external PHY;
> flash + monitor succeeded over USB-C.)

## Verification checklist (per board)

- [ ] USB-C data cable recognized by host (device node appears).
- [ ] Flash of a bring-up sample succeeds (`west flash`, or `esptool` from
      the host machine).
- [ ] Monitor shows continuous output from the philosophers sample
      (`west monitor`, or `screen` from the host machine) — proves the
      console path end-to-end without catching the boot moment.
- [ ] Monitor shows the Zephyr boot banner (after a reset + re-attach).
- [ ] Download-mode recovery procedure executed once and documented.
- [ ] (S3 only) UART0 GPIO43/44 recovery path wired and shown to print the
      boot banner.
- [ ] A second developer repeated the procedure unaided (exit-gate criterion).
