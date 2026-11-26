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

## a14 — Studio RPC (P1 bench path — superseded by the merged build)

> **2026-11-26:** the default `tab1a` build now carries the wired OTG
> composite (b06 merged): the RPC transport is the **"ZMK Studio RPC"
> CDC** on USB-C (`zmk,studio-rpc-uart = &studio_cdc_uart`) and P1 UART0
> is the **console/shell**. This section's P1-as-RPC wiring applies to
> pre-merge images (or a `-C` fragment re-choosing `&uart0`); the wire
> framing, harness and §walks are unchanged — only the port moved. The
> live bench walk is the **b06** section below.

The pre-merge product config chose UART0 as the RPC transport
(`zmk,studio-rpc-uart = &uart0`), so a stock build answered RPC on the P1
header. Console/shell stayed on USB-Serial/JTAG: keep the console
capture running (mind the DTR trap above) while the harness talks, the
firmware logs (`LOG_MODULE zmk_studio`) the request/response trail.

Wiring (3.3 V logic adapter only):

| Tab 1a P1 header | USB-TTL adapter |
|---|---|
| IO43 (UART0 TX) | RX |
| IO44 (UART0 RX) | TX |
| GND | GND |

115200 8N1. TX/RX are from the adapter's point of view (cross the pair).
Boot log confirms binding: `UART RPC transport: using device UART_0` +
`RPC transport selected: uart (endpoint transport=…)`. BLE stays up for
HID but the RPC ignores the endpoint while
`CONFIG_ZMK_STUDIO_RPC_TRANSPORT_UART_PREFERRED=y` (drop the flag when
b06 gives the OTG port a CDC-ACM endpoint).

Harness (see `../app/scripts/studio-harness/README.md` for install):

```sh
cd zmk/app/scripts/studio-harness && npm install && npm run framing-test
node src/cli.js --port /dev/cu.usbserial-XXXX info        # transport proof
node src/cli.js --port ... selftest 2>&1 | tee ../../esp-port/test-results/a14-$(date +%F)-selftest.md
```

`selftest` walks: device info → lock → screen list/navigate (+
`screen_changed`) → `set_time` shows synced → theme live set/restore (+
event) → stats config + data frame → pomo config/start/cancel (+ state
event) → assets list/activate → **a15 upload trio**: 64 KiB upload →
list/generation-bump → activate live, corrupt-stream upload refused
(`committed=false`, live art survives), delete → manifest rewrite →
built-in fallback reasserts → keymap rebind round-trip (stock keymap
RPC, a13 positions) → macro rename round-trip. Steps that need an
on-glass look print an `owner check` line — record what the panel did
next to each, per `format.md`.

Expected failure modes: no response at all → TX/RX crossed wrong or
adapter is 5 V; `Failed to select a transport` → chosen node missing
(rebuild pristine); MetaError `UNLOCK_REQUIRED` → run `lock` first.

## b06 — Studio RPC + HID over the USB-C OTG port (no fuse) — **merged into the default build 2026-11-26**

`./build.sh tab1a` + `./flash.sh tab1a` now put the
**HID-keyboard + CDC-ACM composite** on the USB-C port (GPIO19/20 → the
DWC2 OTG controller) in every image — the separate `tab1a-usb` variant
and its `cobantab1a-usb.{conf,overlay}` fragments are gone (merged into
`cobantab1a.{conf,overlay}`; see b06 Status 2026-11-26). One cable carries
wired typing *and* Studio RPC — the harness talks to the "ZMK Studio RPC"
CDC serial device. Plan of record:
[`docs/b06-tab1a-cdc-acm-usb-plan.md`](../../docs/b06-tab1a-cdc-acm-usb-plan.md)
rev 2; the eFuse question behind it:
[`docs/b06-efuse-usb-phy-sel-report.md`](../../docs/b06-efuse-usb-phy-sel-report.md).

**No eFuse is involved.** At reset GPIO19/20 still belong to
USB-Serial/JTAG (ROM download + `./flash.sh tab1a` keep working, even to
recover a wedged USB build — that is the whole reversibility story); the
device-stack init hands the pads to OTG (`usb_wrap` `OTG_CONF.pad_enable`),
and **any reset reverts them**. Never `espefuse burn_efuse` anything.
Exception at boot: flashing while the running app already holds the CDC
open needs **BOOT+RST** once (ROM instead of app on the pads).

Port map (single build — this table is now the whole truth):

| Port/pins | boot / ROM | running app |
|---|---|---|
| USB-C (19/20) | Serial/JTAG (ROM loader, `./flash.sh tab1a`) | HID keyboard + **CDC-ACM ("ZMK Studio RPC")** |
| P1 UART0 (43/44) | — | **console + shell** @ 115200 (+ ROM messages) |

Walk (all reversible — a power-cycle hands the pads back to the ROM
loader; `./flash.sh tab1a` re-flashes over it):

1. `espefuse.py --chip esp32s3 -p <usb-serial-port> summary` **read-only** →
   `test-results/` (date code + `USB_PHY_SEL=0` baseline; expect the
   `DIS_USB_OTG_DOWNLOAD_MODE` state documented in the fuse report §5).
2. Flash: `./flash.sh tab1a` (Serial/JTAG path — attach happens at reset,
   before the app grabs the pads; BOOT+RST if a running CDC app holds them).
3. Boot log on the **P1 UART0** console: `RPC transport selected: uart`
   at boot — `UART_PREFERRED=y` pins the (now CDC-backed) uart transport
   endpoint-independently. The endpoint-keyed variant (BLE → `gatt`,
   USB → `uart`, pin dropped) is the unproven path behind bug #3 — flip
   the pin in the conf as the first probe if the harness gets no reply
   (`docs/wave2-implementation-report.md` §3.2/§7).
4. Enumerate: `system_profiler SPUSBDataType` → one composite with a
   keyboard and a CDC-ACM interface (macOS names it by ZMK's descriptors).
5. `node src/cli.js --port /dev/cu.usbmodemXXXX selftest` — the entire a14
   acceptance walk now rides the OTG cable; transcript →
   `test-results/a14-b06-<date>-selftest.md`. Then the b06 extras: type
   while BLE is also paired (plug switches the HID endpoint, unplug falls
   back — `zmk endpoint` output), `stats push --demo --hz 50 --for 60`
   during typing (no key drops), host sleep→wake (CDC RX must re-arm — that
   is carried patch `d566a7f1b8` + the `uart_rpc_listener` in
   `uart_rpc_transport.c` doing their job).
6. **Reversibility proofs, log them:** host-sleep/resume and crash-loop
   recovery must both restore with a bare power-cycle; finally power-cycle →
   Serial/JTAG console + `./flash.sh tab1a` work with zero intervention.
7. Record per `format.md`; tick the compat-matrix tab1a USB_PHY_SEL column
   ("19/20 → OTG at runtime on tab1a; reverts at reset; eFuse
   untouched").

Expected failure modes: OTG port dead while the P1 console shows the app
running → the `usb_wrap` pad hand-off isn't reaching hardware (Phase-1
fallback: `esp-port/patches/0007-*`, then rebuild); enumerates but no RPC
→ endpoint didn't switch to USB (`zmk endpoint select usb` to isolate);
CDC port appears then vanishes on suspend → §5 walk item 5.

## a15 — bulk asset upload (chunked + CRC)

Prereq: a14 §walk green; `assets.bin` (v2, gen 1 — `build.sh tab1a`
regenerates `firmware/assets.bin`) flashed at `0xda8000`. The 60 s/1 MB
acceptance bar runs on the **b06 CDC path** (UART0@115200 is ~90 s of
pure wire time for 1 MB — that leg records a baseline instead):

**A. CDC acceptance (merged `tab1a` build, OTG cable, `/dev/cu.usbmodemXXXX`):**

1. `node src/cli.js --port ... lock` then a 1 MB upload:
   `node src/cli.js --port ... assets upload ~/Downloads/1mb.gif --fps 12`
   — expect progress ticks (device sends `upload_progress` every 32 KiB),
   `end: committed=true`, a KB/s line; **wall-clock < 60 s**. Record it.
2. `assets list` shows the new entry, `generation` advanced, `free`
   shrank by ≈ the file size; `assets activate <name>` → the background
   swaps on glass within ~200 ms (this is the store-reprobe + epoch
   guard path working live).
3. Display responsiveness during transfer: watch the console clock page
   (or `stats push --demo` while uploading) — frames keep ticking; the
   writer runs on a preempt-low workqueue, never the display queue.
4. Bit-flip: `assets upload ~/Downloads/1mb.gif --corrupt` →
   `committed=false error="crc mismatch"`, list unchanged, live art
   unaffected on glass (commit-last: the manifest was never rewritten).
5. Power-loss drill: start an upload, yank power mid-stream → reboot →
   boots with the previous generation intact (built-ins + earlier
   uploads play; the partial blob is orphaned, harmless until GC).
6. Resume drill: start an upload, Ctrl-C the CLI → `assets status`
   shows `{active, name, accepted}` → `assets upload <same file>
   --resume` continues at `accepted` and commits. (Cross-reboot resume
   is explicitly v2 — after a reboot, just re-upload.)
7. Delete + GC: `assets delete <name>` → gone from list, built-in art
   (if the name shadowed one) reasserts; fill the partition with a few
   replace-uploads, then `assets gc` → `gc_running` progress
   notification, `free` recovers, all assets still play (per-blob CRCs
   re-verified during relocation — a failed copy abandons before commit).

**B. UART baseline (stock `tab1a` build, P1 header):**

Repeat 1 once at 115200 and once with `--baud 921600` (uart0 handles
it; this is a spot-check, not a product default). Record both KB/s
figures next to the CDC number in `test-results/a15-<date>.md` — this
is the "before/after throughput vs pad10a 25 B HID chunks" evidence.
At 115200 expect ~10 KB/s (vs pad10a's ~0.5 KB/s effective).

Console noise during 1 MB: the `zmk_studio` log lines are per-request —
quiet it (`logs disable zmk_studio`) if the UART console capture can't
keep up; it never affects the transfer itself.
