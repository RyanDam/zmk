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

  Easiest: `bash esp-port/build_esp32.sh [c3|s3|all] [flash]` sets these for
  you and does a clean build (see `../toolchain-esp32.md`). The per-board
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

Console/flash path: USB Serial/JTAG on GPIO18/19 (USB-C). No UART pins needed.

```sh
export ZEPHYR_TOOLCHAIN_VARIANT=espressif
export ESPRESSIF_TOOLCHAIN_PATH=/opt/espressif/tools

# 1. Build a minimal sample (first-time bring-up).
#    Note: the esp32c3_supermini board only exists from Zephyr 4.4 (Phase 5+).
#    On the current 4.1 baseline use the pin-compatible devkitc board.
west build -b esp32c3_devkitc/esp32c3 -d build/c3-hello zephyr/samples/hello_world

# 2. Flash (esptool via the Zephyr runner; board is held in download mode
#    automatically by the USB Serial/JTAG controller on reset)
west flash -d build/c3-hello

# 3. Monitor
west monitor -d build/c3-hello
```

Expected: USB CDC serial device appears on plug (`ls /dev/ttyACM*` on Linux,
`/dev/cu.usbmodem*` on macOS); boot banner `*** Booting Zephyr OS build ...`
in the monitor.

Download-mode recovery (if the board bricked): hold **BOOT** (GPIO9) while
pulsing **RESET**, release BOOT; the ROM loader takes over on USB-C.

> **TODO (hardware):** record the board's actual USB device ID/vendor string
> and whether a data cable was required; confirm BOOT/RESET silkscreen
> positions in `reference-boards/`.

## ESP32-S3 SuperMini

Console/flash path: USB Serial/JTAG on GPIO19/20 (USB-C). **Recovery console:**
UART0 on GPIO43 (TX) / GPIO44 (RX) — keep these two pins free of the matrix
fixture.

```sh
export ZEPHYR_TOOLCHAIN_VARIANT=espressif
export ESPRESSIF_TOOLCHAIN_PATH=/opt/espressif/tools

# 1. Build (no upstream S3 SuperMini board exists yet — for Phase 0 bring-up
#    use the closest upstream board, e.g. esp32s3_devkitc; the dedicated
#    esp32s3_supermini board lands in Phase 6). The S3 is dual-core, so a
#    core qualifier is required: procpu = primary core 0 (appcpu = core 1).
west build -b esp32s3_devkitc/esp32s3/procpu -d build/s3-hello zephyr/samples/hello_world

# 2. Flash
west flash -d build/s3-hello

# 3. Monitor
west monitor -d build/s3-hello
```

Expected: same as C3 — USB CDC device on plug, Zephyr boot banner.

Download-mode recovery: hold **BOOT** (GPIO0) while pulsing **RESET**.

UART0 recovery (last resort, e.g. after a bad USB-related firmware in Phase 8):
wire GPIO43 (TX) → RXI and GPIO44 (RX) → TXI of a 3.3 V UART bridge (USB-UART,
115200 8N1, no flow control); the ROM loader and Zephyr console both fall back
to UART0 when USB Serial/JTAG is not responding.

> **TODO (hardware):** confirm the USB-C connector is actually routed to
> GPIO19/20 on the reference sample (continuity test + boot log), check for an
> external USB PHY (affects Phase 8), and record eFuse `USB_PHY_SEL` state
> (read-only: `esptool.py efuse dump` or ESP-IDF `efuse_summary.py`). Never
> write eFuses on a development board.

## Verification checklist (per board)

- [ ] USB-C data cable recognized by host (device node appears).
- [ ] `west flash` of the hello_world sample succeeds.
- [ ] `west monitor` shows the Zephyr boot banner.
- [ ] Download-mode recovery procedure executed once and documented.
- [ ] (S3 only) UART0 GPIO43/44 recovery path wired and shown to print the
      boot banner.
- [ ] A second developer repeated the procedure unaided (exit-gate criterion).
