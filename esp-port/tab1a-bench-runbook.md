# Coban Tab 1a — bench runbook (c02 qualification + h01 dev harness)

Everything on this page is **bench work** — the parts a code-side agent cannot
verify. Each section ends with the record to write into
[`test-results/`](test-results/format.md) (naming/format per `format.md`).
Board: Tab 1a proto #1 (`compat-matrix.md`). Findings history:
[`axs15231b-bringup-findings.md`](axs15231b-bringup-findings.md).

## Console capture — read this first (DTR trap)

The USB-Serial/JTAG port **resets the S3 whenever the port is reopened with
DTR asserted** (`rst:0x15 USB_UART_CHIP_RESET`). A capture script that
reopens the port periodically looks *exactly* like the c02 "board halts"
symptom (findings §19 lesson, `:1158-1163`, `:1208-1210`).

Rules:

1. Open the port **once** per boot, in one long-lived process:
   `cat "$(ls /dev/cu.usbmodem* | head -1)" > run.log &` (port name is NOT
   stable across replugs — list, don't hardcode; findings `:630-631`).
2. Never re-`open()` the port after the flash step; `./flash.sh tab1a`
   hard-resets the board itself, then start the capture exactly once.
3. `west monitor -d build/cobantab1a --no-reset` is the sanctioned long-lived
   alternative (no auto reset on open).
4. A picture surviving a reflash means the new firmware didn't touch the
   panel (bringup README reset rule).

## c02 — storage + BLE qualification sequence

Pre-flight (code-side, one-time): `CONFIG_ESPTOOLPY_FLASHSIZE_16MB` is now in
the board defconfig, and `CONFIG_ZMK_TAB1A_DEBUG` forces `CONFIG_SETTINGS` on
independently of BLE (`app/src/tab1a/tab1a_debug.c`).

**Step A — storage-only repro (no BLE, no blobs).**

```sh
sh zmk/esp-port/build_cobantab1a.sh -C zmk/esp-port/bringup/c02-storage-only.conf
./flash.sh tab1a
# capture once; within ~5 s expect the `c02dbg:` block:
#   geometry line (ESPTOOLPY_FLASHSIZE / FLASH_SIZE)
#   storage area offset=0xfb0000 size=196608 (compare partitions_0x0_default_16M.dtsi:49-53)
#   settings_save_one -> <code>    (261/-5 path is the repro)
```

If `settings_save_one -> 0` and the round-trip prints OK, the storage bug is
**already fixed** by the 4.5 pin move or the flash-size defconfig fix — mark
acceptance box 2 accordingly and note which config flipped it.

**Step B — full repro with BLE.**

```sh
west blobs fetch hal_espressif   # BLE needs libble_app.a (findings §11, :634-639)
sh zmk/esp-port/build_cobantab1a.sh -C zmk/esp-port/bringup/c02-ble-repro.conf
./flash.sh tab1a
# capture from cold boot (start capture BEFORE power-cycling, or re-capture after
# a USB unplug — remember the panel reset rule)
```

What to capture into `test-results/10-s3-001-tab1a-ble-storage.md`:

- The `c02dbg:` block (storage verdict on the current tree).
- Assert/timeout evidence: search for `Controller unresponsive` /
  `command opcode … timeout` / `BT_ASSERT` — the failure now surfaces at
  `subsys/bluetooth/host/hci_core.c:651/:663` (the recorded `:482` was the
  v4.4.1 line). Note the opcode (expect `0x0c09` HCI_Read_BD_ADDR first).
- Time from boot to halt (the docs disagree: "~13.5 s" vs the 10 s
  `HCI_CMD_TIMEOUT` — re-measure).

**Step C — coexistence run (after A/B pass).** Pair as BLE HID from a host,
type with display + touch live: no assert, no storage errors, no visible
frame stutter → record PASS/PARTIAL with log.

**Step D — RAM / FPS / current records.**

- RAM: build-linker table is printed per build (`Memory region` in the build
  log); PSRAM total from the `esp_psram` boot log; runtime heap: open the
  console and `reboot`… (no runtime reporting exists yet — if needed, enable
  `CONFIG_THREAD_ANALYZER` + `CONFIG_HEAP_STATS`/shell in the repro conf and
  record the method, it becomes the standard).
- FPS: eyeball/stopwatch against the clock screen timer (1 s tick) or add a
  temporary `lv_timer` counter — record method + number per state.
- Current (bench PSU, annotate state per `format.md:54-65`):
  boot idle (display on, BLE idle), typing (BLE active), backlight 0 %/50 %/
  100 % (IO1 PWM proxy for "display power" states — the panel itself has
  no off state in rev A; record what "display off" means for you), touch
  pressed vs idle. → `test-results/11-s3-001-tab1a-ram-current.md`

**Step E — idle-screen policy decision.** Candidates (c02 item §): keep
always-on (status quo), touch-timeout blank, fixed schedule. tab1a rev A has
no key events, so `ZMK_DISPLAY_BLANK_ON_IDLE` can't key off activity. Record
the decision as a comment block in
`zmk-config/boards/shields/cobantab1a/cobantab1a.conf` (c02 acceptance 6).

## h01 — dev harness probe checklist

Build (blob-free is fine — BLE stays off in the base config):

```sh
sh zmk/esp-port/build_cobantab1a.sh \
  -O zmk/esp-port/bringup/tab1a-devkit.overlay \
  -C zmk/esp-port/bringup/tab1a-devkit.conf
./flash.sh tab1a
```

Capture once. ~5 s after boot the harness runs all probes; expected serial
output and wiring:

| Probe | Expected log | Wiring |
|---|---|---|
| WS2812 chase | `WS2812 chase on IO46 (8 px)` then silent | bar DIN→IO46 (P2), **SCLK→IO38** (harness-only, spare TE pin), 5V **from the bar's own supply or USB** + common GND |
| Keys | `harness: key pos N pressed/released` per press | 4 switches+diodes row→col to GND: rows IO7/IO14, cols IO15/IO16 |
| I2S tone | `playing i2s0 tone` → `i2s tone done` | speaker on P5 pads; amp already on SOUND nets (IO2/42/41) |
| VBAT | `VBAT raw <n>` | VBAT pad powered (battery or bench ≤4.2 V); **also** meter the divider: record meter volts + raw → compute ratio → write into `tab1a-hardware-map.md` |
| DS1302 | *(no probe yet — h02)* | pre-wire breakout SCLK=IO17, IO=IO18, CE=**IO6 (R1, unconfirmed)** if desired |

Failures to log verbatim: `led strip update failed` (check 5V common),
`i2s write failed` (GDMA conflict would show at configure), `adc read
failed` (IO5 conflict should be gone since the phantom move — if it returns,
something else claims IO5).

**Trap (already paid for once, 2026-11-21):** `i2s_trigger(START) -> -EIO`
right after configure is *not* hardware — `i2s_esp32` pops the first block
from its write queue at START, so on ESP32 you must `i2s_write()` the first
block(s) **before** START (the harness primes 2 blocks). Worth remembering
for h03's real driver. Likewise `mspi_esp32: Packet 0 failed: -12` means
the internal-RAM bounce heap is exhausted (see the shield conf note), not a
bus fault.

Software-verified on the bare board 2026-11-21 (no wiring): chase ran 55 s
error-free, tone completed all 8 blocks after the prime fix, ADC converted
(floating raw ~185, `uncompensated` warning is benign), matrix produced
zero ghost events. The four rows above need the wiring to close.

Record: `test-results/12-s3-001-tab1a-devkit-probes.md`, one observation per
row above, then tick h01's bench boxes. Board-designer sign-off for the P3
third signal closes h01 acceptance 3 (independent of the probes).
