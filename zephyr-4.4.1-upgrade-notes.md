# Zephyr 4.4.1 upgrade — tracking notes (backlog b01)

## Manifest changes (`app/west.yml`)

| Project | Before | After | Why |
|---|---|---|---|
| `zephyr` | `zmkfirmware/zephyr` @ `v4.1.0+zmk-fixes` | `zmkfirmware/zephyr` @ `v4.4.1+zmk-fixes` | Rebased fork branch (below) |
| `hal_stm32` | `zmkfirmware/hal_stm32` @ `4fcc3a3f` | *(removed — uses 4.4.1 import @ `fc11896dd3`)* | The single ZMK commit ("stm32c0 fix to include HAL_PCD header") is already in the 4.4.1 pin |
| `lvgl` | `zmkfirmware/lvgl` @ `f1db87ee9` | `zmkfirmware/lvgl` @ `48e1ad148` (branch `zmk-v4.4.1`) | ZMK `theme_mono` font patch rebased onto LVGL v9.5.0 (4.4.1 pin `85aa60d18b`); also updates LVGL's internal `lv_theme_mono_init()` caller in `lv_display.c` to the new 5-arg signature |
| `zmk-studio-messages` | `cobanfirmware` @ `main` | unchanged | |
| `name-blocklist` | included `hal_altera` | `hal_altera` removed | No longer present in the 4.4.1 import |

`hal_espressif` comes from the 4.4.1 import: `19f979cfe66bcab09abe3b0b3aa419a664c1606c`.

## Zephyr fork: `v4.1.0+zmk-fixes` (41 commits) → `v4.4.1+zmk-fixes` (17 commits)

### Ported (17)

| Fork commit | Subject | Port note |
|---|---|---|
| `ac2504c91` | soc: nordic: Allow disabling binding header validation | Applied cleanly; not in upstream |
| `f50faa7bc` | cmake: modules: Add new post_boards_shields extension | Applied cleanly; not in upstream; used by `app/boards/post_boards_shields.cmake` |
| `36ba1079b` | boards: arm: Add BoardSource blok RP2040 board | ZMK supports the blok board (`app/boards/boardsource/blok`); board not in upstream |
| `cec120e6a` | arm: stm32: Add DTS describing ROM bootloader for f0 | Part of ZMK STM32 ROM-bootloader feature (ferris etc.) |
| `20fbf5810` | dts: arm: st: Add STM32WB bootloader information | Same feature |
| `db74e1e58` | boards: st: Add boot mode retention support to Nucleo WB55RG | Same feature |
| `f69b56b65` | arm: stm32: Add DTS describing ROM bootloader for f411 | Same feature |
| `5253a2411` | retention: Skip mutex usage when in pre-kernel | Conflict: 4.4.1 refactored `retained_mem_zephyr_ram.c` to `config->lock`; resolved keeping 4.4.1 style inside the pre-kernel guard |
| `4fcff76d0` | soc: arm: stm32: Add ROM bootloader support | Conflicts in Kconfig/Kconfig.defconfig/CMakeLists: `STM32_ENABLE_DEBUG_SLEEP_STOP`/`SWJ_ANALOG_PRIORITY` moved to `soc/st/stm32/common/Kconfig` in 4.4.1; `PHY_INIT_PRIORITY` defconfig entry gone from 4.4.1 — dropped both from the port |
| `61e486680` | boards: seeed: xiao_ble: UF2 runner default, remove ID match | Conflict: 4.4.1 added `nrfutil.board.cmake` include; resolved keeping it, uf2 include first (default runner) |
| `47f69443f` | drivers: retained_mem: add BBRAM driver | Conflict: 4.4.1 added `retained_mem_silabs_buram.c` to CMakeLists; kept both, sorted |
| `f13abe5f1` | tests: drivers: retained_mem: fix compilation | Applied cleanly |
| `c7bb73bfe` | tests: drivers: retained_mem: add BBRAM test | Conflict: 4.4.1 switched test case args to `FILE_SUFFIX` and added `buram` case; kept 4.4.1 style, BBRAM case added |
| `75c39cc7d` | boards: boardsource: Add blok flash controller | Applied cleanly |
| `1d1eb5d96` | Revert "pm: only define slots if CONFIG_PM is enabled" | Still needed: 4.4.1 still has the `#ifdef CONFIG_PM` guard the revert removes |
| `9df4b12b5` | drivers: display: ls0xx: fix releasing SPI bus too soon | Applied cleanly (fixes the VCOM-inversion bus lock ordering) |
| *(new)* | boards: boardsource: Add full_name required by Zephyr 4.4 board schema | 4.4 board.yml schema requires `full_name` for standalone boards |

### Dropped (24)

**Already upstreamed in v4.4.1 (21):**

| Fork commit | Subject | Evidence in 4.4.1 |
|---|---|---|
| `b2aee46d1` | Bluetooth: Controller: Fix peripheral role assertion on conn update | Same commit in 4.4.1 history |
| `7b33a2e53` | drivers: display: ls0xx: add Kconfig setting VCOM thread priority | Same commit in 4.4.1 history |
| `4d1489d5d` | drivers: display: ls0xx: add support for serial VCOM inversion | Same commit in 4.4.1 history |
| `ec3651699` | drivers: flash: stm32g0: Implement option_bytes_write\|read API | Same commit in 4.4.1 history |
| `ec69ba712` | drivers: clock: stm32c0: Add an option to enable CRS for HSI48 | Same commit in 4.4.1 history |
| `9f90a8dfb` | dts: arm: st: c0: Add clk-hsi48 for stm32c071 SOC | Same commit in 4.4.1 history |
| `7e964a723` | include: zephyr: dt-bindings: clock: Add HSI48 support STM32C071 | Same commit in 4.4.1 history |
| `bdf6790ea` | include: zephyr: dt-bindings: Fix USB_SEL mask | Upstreamed as `b7f906aaf3`; 4.4.1 additionally reworked the macro (`d891aad2ed`) |
| `1c698ff90` | drivers: spi: spi_pl022: disable the SSP before reconfiguring | Same commit in 4.4.1 history |
| `9f600baef` | drivers: input: Add sleep-mode-enable property for Pinnacle | Same commit in 4.4.1 history |
| `3bb4355ba` | twister: prefer 'fork' on POSIX to maintain pre-3.14 behavior | Same commit in 4.4.1 history |
| `933505207` | drivers: input: Support invert x/y in rel mode | Same commit in 4.4.1 history |
| `a74258944` | input: pinnacle: Perform software reset on init | Same commit in 4.4.1 history |
| `8ec72ab09` | soc: raspberrypi: rpi_pico: Add RP2 bootloader support | Same commit in 4.4.1 history |
| `6f410790f` | boards: Add Adafruit Metro RP2040 | Same commit in 4.4.1 history |
| `c4469341b` | boards: seeed: Fix XIAO MG24 standard uart pins | Same commit in 4.4.1 history |
| `eed722fee` | Bluetooth: Controller: Fix connection update interval_us variables | 4.4.1 `ull_conn.c` already declares `conn_interval_old_us/new_us` as `uint32_t` (fix landed with the 1 ms-connection rework) |
| `b5b4daac5` | include: zephyr: dt-bindings: Add STM32C0 USB clock selection support | 4.4.1 `stm32c0_clock.h` already defines `USB_SEL` (post-rework macro form) |
| `cad0de364` | dts: arm: st: c0: Add USB device node | 4.4.1 `stm32c071.dtsi` already has the `usb@40005c00` node |
| `95098f069` | drivers: timer: silabs: Fix calculation of next tick | 4.4.1 `silabs_sleeptimer_timer.c` already contains the fixed algorithm (`curr % cyc_per_tick`, `next == 0` guard) |
| `1a3d084ba` | soc: silabs: Use configdefault for default values | 4.4.1 `silabs_s2/Kconfig.defconfig` already uses `configdefault` (and evolved further) |

**Obsolete / superseded (3):**

| Fork commit | Subject | Rationale |
|---|---|---|
| `58a5874a4` | backport ls0xx serial VCOM inversion support to Zephyr 4.1 | Comment-only backport wrapper; 4.4.1 comment already matches the post-change state |
| `ec305a460` | boards: seeed: Add XIAO MG24 | Board exists in 4.4.1 (`boards/seeed/xiao_mg24`) |
| `14138d3be` | tests: kernel: sleep: Add Silabs adjustment to max limit | Merged to upstream main after 4.4.1 (present in `usleep.c` on main); test-only, and ZMK builds no silabs boards |

**Superseded by upstream rework (1):**

| Fork commit | Subject | Rationale |
|---|---|---|
| `10ba6d0cb` | Bluetooth: Controller: Fix prepare pipeline overflow | The unbounded `-EBUSY` prepare deferral this fixed was reworked upstream in 4.4.1 by the "prepare deferred feature" (`c2eb901ea1`, 2025-07-11): `lll_conn_{central,peripheral}_is_abort_cb` now bound deferrals to `*_TRX_BUSY_ITERATION_MAX` (≤4) before returning `-ECANCELED`. Porting the ZMK change would disable the deferral mechanism upstream added for long coded-PHY events. **Action: re-validate ZMK split-central scenarios (2+ connections) in the Phase 4 regression gate; re-apply the fix if lockups recur.** |

## kscan compatibility shim (temporary — removed by a01, done)

Zephyr 4.2 removed the kscan subsystem; ZMK's kscan drivers and consumers still use it.
Phase 1 restored a temporary shim in the ZMK tree:

- `app/module/include/zephyr/drivers/kscan.h` — 4.1 header with the syscall layer
  (`__subsystem`/`__syscall`/`z_impl_*`/generated `syscalls/kscan.h`) removed; plain
  `static inline` wrappers over `kscan_driver_api`.
- `app/Kconfig.kscan_compat` (rsource'd from `app/Kconfig`) — `KSCAN` menuconfig +
  `KSCAN_INIT_PRIORITY` (4.1 values).
- `app/dts/bindings/kscan/kscan.yaml` — 4.1 binding definition (`include: base.yaml`,
  `bus: kscan`) referenced by the six `zmk,kscan-*` bindings.
- `app/module/drivers/kscan/CMakeLists.txt` — `zephyr_library_amend()` →
  `zephyr_library()`: the amend target (Zephyr's own `drivers/kscan` library) no
  longer exists in 4.4.

**a01 (Phase 2) removed the shim and migrated the subsystem to the Input API:**

- All seven `app/module/drivers/kscan/` drivers (matrix, direct, charlieplex, demux,
  composite, mock) are now Input API producers: they emit
  `INPUT_EV_ABS` X (column) / Y (row) + `INPUT_EV_KEY` `INPUT_BTN_TOUCH` + sync via
  `zmk_kscan_input_report()` (new shared header `kscan_input.h`). The kscan
  configure/enable/disable driver API is gone; drivers start scanning autonomously
  (PM resume, or init when PM is off). The ZMK software debounce is preserved.
- `app/src/kscan_sideband_behaviors.c` and the composite driver are Input-event
  wrappers: they register `INPUT_CALLBACK_DEFINE` on the inner device(s) and
  re-emit (offset, for composite) on their own device.
- `app/src/physical_layouts.c`: the kscan callback path is deleted; the Input path
  is the sole path. Per-layout device resolution: `input` property → legacy `kscan`
  property → `zmk,matrix-input` chosen → `zmk,kscan` chosen.
- **DT compat names are intentionally unchanged** (`zmk,kscan-*`, `zmk,kscan`
  chosen, `kscan` property) so existing board/shield devicetrees (incl. the
  zmk-config coban pads) keep building. A future rename to `zmk,input-*` is tracked
  in the backlog (see `backlogs/`).
- Kconfig: the `KSCAN` symbol and its `select`s are gone (the "Deprecated symbol
  KSCAN is enabled" warning is removed); the drivers now `select INPUT` instead,
  and `ZMK_KSCAN_INIT_PRIORITY` (default 90) replaces `KSCAN_INIT_PRIORITY`.
- Input event processing mode: `app/prj.conf` sets
  `CONFIG_INPUT_MODE_SYNCHRONOUS=y` instead of Zephyr's default thread mode —
  rationale and trade-offs in [Input event processing mode](#input-event-processing-mode-synchronous) below.
- Deleted: `app/Kconfig.kscan_compat`, `app/module/include/zephyr/drivers/kscan.h`.
- Zephyr 4.4 macro gotcha hit during the port: `COND_CODE_1`/`UTIL_AND` flags must
  be literal 0/1 tokens (token-pasted), and **both branches must be parenthesized**
  (the else-branch is debracketed via `__DEBRACKET`); bare `DT_CHOSEN(...)` branches
  silently mis-route or fail with `__DEBRACKET` undeclared. Node IDs are tokens,
  not integers — `node_id >= 0` comparisons don't work; use `DT_NODE_EXISTS` /
  property-length booleans.
- Two more macro gotchas hit during the double-check: (1) `INPUT_CALLBACK_DEFINE_NAMED`'s
  `name` argument is in a `##` context (`_input_callback__##name`), so a computed name
  like `_CONCAT(_cb_, n)` is spliced **unexpanded** (`_input_callback___CONCAT` plus
  leftover tokens) — define the `STRUCT_SECTION_ITERABLE(input_callback, ...)` entry
  directly and paste the instance/node token at the call site instead. (2) A `/* */`
  comment inside a multi-line `#define` body must end **every** line with `\`; otherwise
  the directive terminates at the first comment line and the rest of the body silently
  lands at file scope (where instance-parameter flags evaluate to 0 and `COND_CODE_1`
  drops the branch without error). (3) **A missing-property length is a poison token,
  not 0.** `DT_INST_PROP_LEN(inst, prop)` for a property the node does *not* have
  expands to an **undefined identifier** (e.g. `DT_N_S_<node>_P_<prop>_LEN`), not to
  `0`. That is fine as a bare `COND_CODE_1` flag (non-literal -> else branch), but it is
  *fatal* inside `UTIL_OR`/`UTIL_AND`: `UTIL_BOOL(<undefined>)` is `1`, so
  `UTIL_OR(<undefined>, x)` returns the undefined token itself and the whole expression
  becomes non-literal, making a downstream `COND_CODE_1` silently take its else branch.
  This bit `LAYOUT_HAS_INPUT_DEV()` in `physical_layouts.c`: layouts that resolve their
  input device via the `zmk,kscan`/`zmk,matrix-input` *chosen* (no explicit
  `input`/`kscan` property on the layout node - e.g. cobanpad16a/12b, ferris) silently
  lost both the `.input` field **and** the registered input callback, so the matrix
  driver was never PM-resumed and never scanned (dead keyboard, no log output). The
  existing native_sim matrix test did not catch it because its layout node sets an
  explicit `input = <&mock>` property (a defined length). Fix: test property presence
  with `DT_INST_NODE_HAS_PROP(inst, prop)` (a clean literal 0/1) instead of
  `DT_INST_PROP_LEN` wherever the result feeds a `UTIL_OR`/`UTIL_AND`/`COND_CODE_1`
  chain.

### Input event processing mode (SYNCHRONOUS)

Zephyr's input subsystem (`zephyr/subsys/input/input.c`) has two event-dispatch
modes, selected by the `INPUT_MODE` Kconfig choice (default: thread mode):

- **Thread mode** (`CONFIG_INPUT_MODE_THREAD`): `input_report()` does
  `k_msgq_put()` into a global message queue (`CONFIG_INPUT_QUEUE_MAX_MSGS`,
  default 16 × 12 B) and a dedicated thread (`K_THREAD_DEFINE`, default
  priority 0, `CONFIG_INPUT_THREAD_STACK_SIZE` default 1024 B) dequeues events
  and invokes the registered callbacks.
- **Synchronous mode** (`CONFIG_INPUT_MODE_SYNCHRONOUS`): `input_report()`
  calls `input_process()` inline — the callbacks run immediately, in the
  caller's context. For ZMK that is the kscan driver's scan work handler
  (system workqueue); the drivers' IRQ handlers only schedule work, they never
  report events directly.

`app/prj.conf` sets `CONFIG_INPUT_MODE_SYNCHRONOUS=y` for all ZMK builds.

**Why synchronous (advantages):**

1. **Behavioral equivalence with the removed kscan path.** Legacy kscan
   callbacks always ran synchronously in the driver's scan context; there was
   no queue or thread in between. Synchronous mode is the faithful port — same
   context, same ordering, same timing — which is a01's core requirement
   ("preserve keyboard behavior").
2. **~1.4 KB less RAM on every board.** Thread mode allocates the input thread
   stack (1024 B) + event msgq (192 B) + thread bookkeeping. On 16 KB-RAM
   boards (ferris, bdn9) that is ~9 % of RAM: it would deepen this branch's
   pre-existing ferris RAM overflow from ~0.6 KB to ~2 KB. On 256 KB nRF52840
   boards it is negligible, but it is still a pure tax with no functional gain.
3. **No event loss.** ZMK drivers report with `K_NO_WAIT`; in thread mode a
   full queue **drops the event** (a `LOG_WRN` is the only trace). A key
   change is a 3-event group (ABS X, ABS Y, BTN_TOUCH+sync), so an N-key
   rollover burst can fill the 16-message queue if the input thread is delayed
   — dropping one event of a group desynchronizes the row/col state.
   Synchronous mode has no queue, so nothing can be dropped.
4. **No scheduler races.** The thread indirection is exactly what caused two
   test failures during the port (the mock's `exit(0)` racing the input
   thread; consumer work queued behind the exit work on sys_workq). Synchronous
   dispatch removes that whole class: the consumer's work is queued before the
   driver moves on.
5. **Deterministic, lower latency** — no scheduling hop per event group.

**Costs (disadvantages):**

1. **Callbacks run in the driver's context and must stay fast and
   non-blocking.** No sleeps, no blocking APIs — otherwise the scan loop
   (debounce timing, scan period) stalls. ZMK's callbacks are safe: the
   physical-layouts callback does `k_msgq_put` + `k_work_submit` (both
   non-blocking, ISR-safe); composite/sideband re-report via `input_report()`
   on the parent device, which in synchronous mode recurses inline — the chain
   is a DAG (child → parent → layout), no cycles.
2. **Callback time is added to the scan cycle.** Negligible for ZMK's
   consumers (microseconds), but it is a constraint on any *future* consumer
   of these input devices.
3. **Latent ISR constraint.** If a driver ever reports from an ISR, callbacks
   run in ISR context and must be ISR-safe. No ZMK kscan driver does this
   today (IRQs only schedule work), but it is a rule to keep.
4. **Diverges from the Zephyr default.** A reader of `.config` sees a
   non-default choice; this section is the justification. Zephyr's own input
   drivers (longpress/keymap/double-tap, not used by ZMK) support both modes.
5. **Thread mode is no longer exercised by the test suite.** The native_sim
   suite runs in the mode `app/prj.conf` sets (synchronous), which is what
   production uses — good — but the thread-mode path (a Zephyr mechanism, not
   ZMK code) is untested by ZMK CI.

**Kconfig gotcha hit during the switch:** `ZMK_POINTING`
(`app/src/pointing/Kconfig`) unconditionally did
`select INPUT_THREAD_PRIORITY_OVERRIDE`, but that symbol only exists inside
`if INPUT_MODE_THREAD`. In synchronous mode the select targets a symbol whose
direct dependencies are unsatisfied, and Zephyr's `kconfig.py` turns any such
warning into a hard error (`error: Aborting due to Kconfig warnings`) — so
every board with `CONFIG_ZMK_POINTING=y` (e.g. the coban shields) failed to
configure. Fixed with `select INPUT_THREAD_PRIORITY_OVERRIDE if
INPUT_MODE_THREAD` (the override is meaningless without an input thread
anyway). Rule to keep: any `select` of a mode-conditional Zephyr input symbol
must carry the same condition.

## ZMK tree migration changes (4.2 + 4.3 + 4.4 guides)

- **board.yml schema (4.4):** added `full_name` to all 21 standalone ZMK board.yml
  files (`app/boards/**`, `app/module/boards/**`). Board extensions (`extend:`)
  unchanged. Display-only field; values set to the board name.
- **BT host (4.4):** `BT_LE_ADV_OPT_USE_NAME` / `BT_LE_ADV_OPT_FORCE_NAME_IN_AD`
  removed upstream (`fbd7acec25`). `app/src/ble.c` now puts the device name
  explicitly in the advertising data (`BT_DATA_NAME_COMPLETE`), flags first.
- **Deprecated legacy USB stack:** still functional in 4.4.1 (`DEPRECATED`); ZMK
  keeps selecting `USB_DEVICE_STACK` — migration to `device_next` is Phase 3 (a02).

### Build-matrix migration fixes (found by the 18-build core-coverage matrix)

- **Snippets (4.4):** the application source dir is no longer added to
  `SNIPPET_ROOT` by default (4.4 migration guide). `app/CMakeLists.txt` now sets
  `set(SNIPPET_ROOT "${CMAKE_CURRENT_LIST_DIR}")` before `find_package(Zephyr)`,
  so `app/snippets/*` (studio-rpc-usb-uart, zmk-usb-logging, …) are discovered.
- **Dead `BT_CTLR` Kconfig (4.4 strictness):** `config BT_CTLR / default BT` blocks
  removed from 12 board `Kconfig.defconfig` files (nrf52840_m2, nice60, nrfmicro,
  bluemicro840, pillbug, adv360pro, glove80, bt60/bt65/bt75, s40nc, corneish_zen).
  `BT_CTLR` was never a real Zephyr symbol (only `HAS_BT_CTLR`/`BT_CTLR_*` exist, in
  4.1 and 4.4 alike) — harmless in 4.1, but 4.4 aborts on the "defined without a
  type" Kconfig warning.
- **rp2040 retention dtsi path:** `sparkfun_pro_micro_rp2040_zmk.dts` include moved
  `arm/raspberrypi/rp2040-boot-mode-retention.dtsi` →
  `vendor/raspberrypi/rp2040-boot-mode-retention.dtsi` (upstream relocated the file).
- **xiao_ble CDC ACM serial backend (4.4):** the xiao_ble board's
  `BOARD_SERIAL_BACKEND_CDC_ACM` now defaults its console to the `device_next` USB
  stack, which double-instantiates the CDC ACM device against ZMK's legacy stack
  (`__device_dts_ord_*` multiple definition). ZMK uses no board serial console, so
  `xiao_ble_zmk_defconfig` sets `CONFIG_BOARD_SERIAL_BACKEND_CDC_ACM=n`.
- **ssd1306 display compatible (4.2 driver rework):** `solomon,ssd1306fb` (framebuffer
  driver) is gone; the new display-controller driver uses `compatible =
  "solomon,ssd1306"`. Migrated all 20 ZMK display shields (kyria, corne, zodiark,
  lily58, snap, nibble, murphpad, tidbit, knob_goblin, leeloo_micro, microdox,
  waterfowl, elephant42, lotus58, jorne, splitkb_aurora_*, …). All nodes already
  carry the new binding's required properties. Zephyr 4.4's LVGL glue
  (`zephyr/modules/lvgl/lvgl_display.c`) uses the display-controller API
  (`display_write`), so the new driver is the correct backend.
- **Studio sensor subsystem:** `sensor_subsystem.c` referenced
  `CONFIG_ZMK_KEYMAP_SENSORS_MAX_BINDINGS`, which only exists when the keymap DT has
  a `zmk,keymap-sensors` node. Added an `#ifdef` fallback (0) for keymaps without
  sensors — latent bug, surfaced by the studio matrix builds.

### Build warning cleanup (coban pads: 320 → 99)

The coban builds emitted 320 warnings on 4.4.1. Breakdown and resolution:

- **HID usage macro redefinitions (220 warnings, 11 macros × 20 TUs):** Zephyr 4.4
  added HID usage codes to `<zephyr/usb/class/hid.h>`, duplicating ZMK's
  `dt-bindings/zmk/hid_usage.h` / `hid_usage_pages.h`. Two dead ends found along
  the way: (a) `#ifndef` guards on ZMK's side are NOT sufficient — the warning
  simply moves to Zephyr's header in TUs where ZMK's headers are included first;
  (b) including Zephyr's `hid.h` from ZMK's dt-bindings headers breaks the
  **devicetree** build — `app/dts/behaviors/*.dtsi` include
  `dt-bindings/zmk/keys.h` → `hid_usage*.h`, and Zephyr's `hid.h` contains C
  code (`enum hid_kbd_code`, …) that dtc cannot parse. Final fix:
  - Removed the 10 duplicate `HID_USAGE_SENSORS*` definitions (values were
    identical; nothing in ZMK's C code or the keymaps uses them) from
    `hid_usage.h` / `hid_usage_pages.h`, with comments pointing to
    `<zephyr/usb/class/hid.h>`. The dt-bindings headers stay pure-`#define`
    (DTS-safe).
  - `HID_USAGE16` was a *semantic* clash: ZMK's 2-arg report-descriptor item
    macro vs Zephyr 4.4's 1-arg `HID_USAGE16(idx)`. Removed ZMK's
    `HID_USAGE16`/`HID_USAGE16_SINGLE` and switched the single usage (report
    descriptor in `zmk/hid.h`, consumer AC-Pan item) to Zephyr's
    `HID_USAGE16(idx)` — identical descriptor bytes.
- **Deprecated `FIXED_PARTITION_*` macros (2 warnings):** `reset_settings_nvs.c`
  used deprecated `FIXED_PARTITION_ID` → `PARTITION_ID` (4.4 replacement); same
  one-line fix in `boot/stm32_enforce_nboot_sel.c`
  (`FIXED_PARTITION_DEVICE` → `PARTITION_DEVICE`).
- **The remaining 99 warnings are all deprecation notices** from the legacy USB
  device stack (`CONFIG_USB_DEVICE_STACK`) and the temporary kscan shim — none
  are ZMK bugs, and all are scheduled to disappear (detailed below). They were
  deliberately NOT suppressed: `-Wno-deprecated-declarations` would only silence
  the function-deprecation class (the pragma-based ones cannot be filtered, per
  Zephyr's own comment in `include/zephyr/toolchain/gcc.h`) and would mask real
  deprecations in our own code.

#### The remaining 99 warnings, in detail

| Category | Count | Mechanism | Emitted from | Removed by |
|----------|-------|-----------|--------------|------------|
| Legacy USB **function** deprecations: `usb_write`, `usb_transfer*`, `usb_dc_ep_*`, `usb_hid_*`, `usb_enable`, `usb_cancel_transfer*`, `hid_int_ep_write`, … | 77 | `-Wdeprecated-declarations` on the legacy stack's API (deprecated in Zephyr 4.4 in favor of the `device_next` stack) | Mostly Zephyr's legacy stack: `subsys/usb/device/usb_device.c` (largest share), `usb_transfer.c`, `class/cdc_acm.c`, `usb_descriptor.c`, `class/hid/core.c`, `drivers/usb/device/usb_dc_nrfx.c`; plus ZMK's USB code: `app/src/usb_hid.c`, `app/src/usb.c` | **Phase 3 (a02)** — USB `device_next` migration |
| `USB_TRANS_READ` / `USB_TRANS_WRITE` / `USB_TRANS_NO_ZLP` **macro** deprecations | 19 | Zephyr's `__DEPRECATED_MACRO` mechanism (`_Pragma("GCC warning ...")`); the deprecated aliases are defined in `zephyr/include/zephyr/usb/usb_device.h` (lines 376–378) | Zephyr's *own* legacy stack files: `class/cdc_acm.c` (8), `usb_descriptor.c` (5), `usb_transfer.c` (4), `class/hid/core.c` (2) — an upstream wart: the legacy stack uses its own deprecated aliases | **Phase 3 (a02)** — these files stop compiling once the legacy stack is gone |
| "Deprecated symbol … is enabled" Kconfig warnings | 3 | kconfiglib warning for `--- deprecated` Kconfig symbols that are enabled | `USB_DEVICE_STACK` + `USB_DEVICE_DRIVER` (legacy stack) → **a02**; `KSCAN` (superseded by the input subsystem; enabled by our temporary compat shim, see above) → **a01** | **Phase 3 (a02) + a01** |

Tally check: 77 + 19 + 3 = 99 (identical for cobanpad12b and cobanpad16a).

**Re-verification command** (after a01/a02 land):
```sh
grep -c 'warning:' <coban-build>.log   # expect 0
```

## Environment / CI

- Zephyr SDK: 0.16.9 → **1.0.1** (minimum for 4.4).
- Python: **3.12** required; build deps from `zephyr/scripts/requirements-base.txt`
  (`jsonschema`, `pykwalify`, `pyelftools`, …).
- CI image: `docker.io/zmkfirmware/zmk-build-arm:4.4-branch` (upstream publishes a
  4.4 image: Zephyr 4.4.0, SDK 1.0.1, built 2026-06-11); devcontainer:
  `docker.io/zmkfirmware/zmk-dev-arm:4.4-branch`.

## Fork hosting (resolved)

- `zmkfirmware/zephyr` is stale (last upstream sync ~2021, no v4.4.1 history) and
  **RyanDam has no push access** (403 on dry-run). Decision: host the branches in
  forks under the user's own account.
- Forks created via GitHub API (full upstream history inherited):
  - `RyanDam/zephyr` ← fork of `zephyrproject-rtos/zephyr` (has `v4.4.1` tag)
  - `RyanDam/lvgl` ← fork of `lvgl/lvgl` (has `v9.5.0` tag)
- `app/west.yml`: `zephyr` and `lvgl` projects now use the existing
  `cobanfirmware` remote (url-base `https://github.com/RyanDam`); default
  repo-path = project name.
- **Pushed and verified on the remotes:**
  - `RyanDam/zephyr` `v4.4.1+zmk-fixes` @ `da9b77b04d` (17 commits above `v4.4.1`)
  - `RyanDam/lvgl` `zmk-v4.4.1` @ `48e1ad148` (single commit on `v9.5.0`)
  - `RyanDam/zmk-studio-messages` `main` @ `79e59e6` — the branch's studio WIP
    landed here: touchpad protos (incl. frequency/joystick `SetFrequency` RPCs)
    and the `touchpad.options` nanopb options (`max_count:11` bindings,
    `max_size:32` layer name). Note: the options file is **required** for the
    firmware build — without `max_size` on `Layer.name`, nanopb generates
    `pb_callback_t` and `touchpad_subsystem.c` fails to compile.

## Validation status

- [x] nice_nano + corne_left (Nordic USB+BLE, split) builds on 4.4.1
- [x] core-coverage matrix (18 builds): **16/18 pass** on 4.4.1. The 2 failures
      (`stm32-studio` = bdn9, `samd21-studio` = seeeduino_xiao) are **pre-existing
      on this branch, not 4.4 regressions**: the branch's studio-only dynamic-macro
      feature (`ZMK_BEHAVIOR_DYNAMIC_MACRO`, default y) allocates
      `dynamic_macros[16][64]` × 28 B ≈ **28 KB of bss**. Confirmed by diagnostic
      builds with the feature disabled: XIAO then **passes**; bdn9 still overflows
      by ~3.5 KB (the branch's other new studio subsystems — sensor/touchpad/macro —
      plus keymap settings storage). Upstream `main` (what CI builds) has none of
      these, so the combos fit there. Mitigation is branch-feature tuning (smaller
      `ZMK_DYNAMIC_MACROS_COUNT`/`MAX_STEPS`, per-board overrides), out of Phase 1
      scope — candidate for a separate backlog item.
- [x] native_sim test suite: **246 PASS / 0 FAIL / 2 PENDING** (248 total; the
      2 PENDING are pre-existing `pending` markers: `studio/remove-layer/
      remove-default-layer`, `modifiers/implicit/kp-mod1-dn-mod2-dn-mod1-up-mod2-up`).
      Run sequentially (`J=1`) due to the parse_syscalls race below.
- [x] 7 round-1 matrix builds re-verified on the final committed tree — all pass:
      nice60, bdn9, proton_c/clueboard_california, planck, nice_nano/kyria_left
      (pointing), nice_nano/romac_plus (underglow), nice_nano/lily58+nice_view.
- [x] cold-cache CI run: fresh workspace (`west init -l app && west update &&
      west zephyr-export`), every project fetched from the real GitHub remotes
      (all three forks resolve). `nice_nano/corne_left`, `cobanpad16a` and
      `cobanpad12b` all build. Cosmetic note: zephyr uses `clone-depth: 1`, so
      cold builds embed the bare-hash version string (`OS build da9b77b04dd3`)
      instead of `v4.4.1-17-gda9b77b04dd3` — `git describe` can't run in a
      shallow clone. ~8 FLASH bytes, expected in CI.

**Test-runner race (pre-existing, 4.1 and 4.4 alike):** `parse_syscalls.py`
`os.walk`s the app source dir — which includes `app/build/tests/*` — then opens
every collected `.c`/`.h` file. With the default `J=4` parallel test builds, a
sibling test's build can delete a file (e.g. CMake `CompilerIdC` artifacts)
between the walk and the open → `FileNotFoundError` → spurious "did not build".
Same code path in 4.1.0 (`scripts/build/parse_syscalls.py` is structurally
identical). Workaround used for validation: clean `app/build/tests` and run with
`J=1`. Worth a separate hardening item (skip build dirs in the walk, or tolerate
vanishing files).
