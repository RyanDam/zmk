# b05 — cobantab1a: ESP32-S3 display module board + AXS15231B QSPI display/touch shield

- **Category:** b (Build, deploy & config)
- **Severity:** Medium
- **Status:** IN_PROGRESS
- **Effort (est):** L

## Status (2026-11-14)

Phases 1–4 are **implemented and compile-verified** (branch `feat/cobantab1a` in both
repos; nothing committed yet — pending user review):

- **Phase 1 (board def)**: `app/boards/others/cobantab1a/` (14 files). Bare board build
  verified: FLASH 211828 B, `ext_dram_seg: 1216 KB` (system PSRAM heap).
- **Phase 2 (display driver)**: `app/module/drivers/display/axs15231b.c` +
  `Kconfig.axs15231b` + binding `axs,axs15231b.yaml` (+ `vendor-prefixes.txt`: `axs Axsem`).
  Compiles and links (symbols `axs15231b_api` etc. in the ELF).
- **Phase 3 (shield)**: `/workspaces/zmk-config/boards/shields/cobantab1a/` (7 files:
  `.zmk.yml`, `Kconfig.shield`, `Kconfig.defconfig`, `cobantab1a-layout.dtsi`,
  `cobantab1a.overlay`, `cobantab1a.conf`, `cobantab1a.keymap`).
- **Phase 4 (touch driver)**: `app/module/drivers/input/axs15231b_touch.c` +
  `Kconfig.axs15231b_touch` + binding `axs,axs15231b-touch.yaml`. Compiles and links.
- **Phase 5 (partial)**: `esp-port/build_cobantab1a.sh` created; `esp-port/README.md`
  phase table + decisions updated. **No CI entry** (see decision below). Hardware
  bring-up (panel init, LVGL render, touch tracking, backlight, idle blanking,
  RAM/current measurements) is pending.

### Implementation corrections vs. the plan above

- **QSPI wire protocol** (verified against ESP-IDF `esp_lcd_panel_io_spi.c` v5.2 + the
  reference bsp): the 32-bit opcode word `[opcode, 0x00, 0x00, cmd]` is sent in
  **1-wire mode (D0 only)** — not 4-wire; params are **1-wire**; only the color data is
  **4-wire QIO**. CS is held from the command through its params, and from the color
  command through the pixel data (the line mode switches 1-wire → 4-wire mid-transaction
  with CS held). Zephyr SPI API: `SPI_LINES_QUAD` (bits 16–17 of the op flags) +
  `SPI_HOLD_ON_CS` (bit 12) / `SPI_LOCK_ON` (bit 13).
- **RASET is sent** (CASET + RASET + one RAMWR per dirty region). The reference omits
  RASET in QSPI mode because its full-frame writes always start at y=0; ZMK/LVGL writes
  arbitrary dirty rectangles, so RASET is required. `write()` copies the pitched LVGL
  buffer into a contiguous `k_malloc` buffer before one RAMWR transaction.
- **Backlight uses the standard `pwms` DT property** (not a custom `backlight-pwm`):
  `pwms = <&ledc0 0 200000 0>` (LEDC ch0, 5 kHz), `ledc0_default` pinctrl group routes
  CH0 → GPIO1 with `output-enable`. Note: `pwm_pin_configure_dt()` / `PWM_ACTIVE_HIGH`
  **do not exist in Zephyr 4.4.2** — the driver must not call them; pinctrl owns the pin.
- **LVGL memory in PSRAM via custom sections, not DT memory regions.** The ESP32-S3
  linker script has fixed MEMORY regions and already `KEEP(*(.lvgl_buf*))` /
  `KEEP(*(.lvgl_heap*))` into `.ext_ram.data` (→ `ext_dram_seg` → PSRAM). The shield conf
  therefore uses `CONFIG_LV_Z_VDB_CUSTOM_SECTION=y` +
  `CONFIG_LV_Z_MEMORY_POOL_CUSTOM_SECTION=y` (+ `LV_Z_MEM_POOL_SIZE=262144`,
  `LV_Z_VDB_SIZE=20` ≈ 60 KB, `LV_COLOR_DEPTH_16` default). Custom
  `zephyr,memory-region` nodes do NOT work here: they produce orphan sections the ESP32
  linker places in `IDT_LIST` (overflow).
- **Touch driver**: reports via `input_touchscreen_report_pos()` (in
  `zephyr/drivers/input/input_touch.c`, gated by `CONFIG_INPUT_TOUCH` — the driver Kconfig
  must `select INPUT_TOUCH`), not `input_report_touch()`. I2C transfer is
  `i2c_write_read_dt(spec, wbuf, wlen, rbuf, rlen)` (this tree's signature — no
  `struct i2c_msg` array). Read command is 11 bytes
  `{0xB5,0xAB,0xA5,0x5A,0,0,0,0x08,0,0,0}` → 8-byte response; `num=data[1]`,
  `x=((data[2]&0xF)<<8)|data[3]`, `y=((data[4]&0xF)<<8)|data[5]`. Polling: `k_timer`
  10 ms → `k_work` (no INT pin).
- **Phantom kscan**: `zmk,kscan-gpio-matrix` 1×1 — row GPIO2 (pull-up), col GPIO5 —
  inert, never fires; `zmk,matrix-transform` 1×1 `RC(0,0)`; `zmk,physical-layout`
  (display-name + transform + kscan + 1 key) chosen as `zmk,physical-layout`; keymap =
  1 layer, 1 binding `&none`. `ZMK_KEYMAP_LEN` = transform map length = 1, so the keymap
  must have exactly 1 binding.
- **Zephyr 4.4.2 API facts learned**: no `DT_PROP_EXISTS` (use `DT_NODE_HAS_PROP`,
  `#if`-safe); pixel enum is `PIXEL_FORMAT_RGB_565`; there is **no** `SCREEN_INFO_RGB`
  flag (plain RGB LCD → `caps->screen_info = 0`); I2C flag is `I2C_MSG_READ`; the ESP32
  I2C driver reads bus speed from `clock-frequency` on the bus node;
  `zephyr,touch` chosen is consumed by LVGL's `LV_Z_POINTER_FROM_CHOSEN_TOUCH` (default y
  when the chosen exists) — that is the touch → LVGL pointer path.
- **CI decision**: no `app/core-coverage.yml` entry. CI has no access to the external
  ZMK config repo, and the coban product shields (cobanpad12b/16a, cobantab1a) live there
  — none of them are CI targets. The canonical build path is
  `esp-port/build_cobantab1a.sh`.
- **Known risk (unresolved, hardware-verify)**: `spi_esp32_spim.c` runs a dummy
  `spi_esp32_transfer()` inside `spi_esp32_configure()` for mode 3 + GPIO CS *before* the
  CS assertion — the first 1-wire command may be clocked with CS deasserted. If the panel
  fails to init on hardware, this is the first suspect; the dedicated SPI-HAL QSPI driver
  remains the documented fallback.

## Problem

The cobantab1a module — a custom ESP32-S3 board (same silicon and display wiring as the
Guition JC3248W535 reference: ESP32-S3-WROOM-1 N16R8, 16 MB flash, 8 MB octal PSRAM,
3.5" 320×480 AXS15231B QSPI touch display) — has no ZMK support. There is no board
definition, no driver for the AXS15231B panel (QSPI) or its in-cell touch (I2C), and no
shield definition. The reference project at `/workspaces/ref/JC3248W535EN` (ESP-IDF +
LVGL 8.4) is the source of truth for the panel init sequence and touch protocol.

This item is the **display class** of Phase 9b ([a04](a04-esp-battery-display-rgb-peripherals.md)):
one peripheral class per change set, with measured RAM/current impact and no BLE/HID
interference.

## Evidence

- `/workspaces/ref/JC3248W535EN/src/esp_bsp.c:33-66` — panel init sequence actually used
  on the JC3248W535 panel (differs from the Espressif driver's built-in default at
  `src/esp_lcd_axs15231b.c:199-232`; the bsp sequence is the one to port).
- `/workspaces/ref/JC3248W535EN/src/esp_bsp.c:26-39` — QSPI protocol: 32-bit opcode
  (`0x02` write-cmd / `0x32` write-color in the top byte) + 8-bit command; in QSPI mode
  `RASET` is skipped (`src/esp_lcd_axs15231b.c:306-313`).
- `/workspaces/ref/JC3248W535EN/src/esp_bsp.h:33-46` — pinout: QSPI CS 45, PCLK 47,
  D0–D3 21/48/40/39, TE 38, BL 1 (PWM 5 kHz), touch I2C SDA 4 / SCL 8; **RST and touch
  INT/RST are NC** (software reset via `SWRESET` 0x01; touch is polled).
- `/workspaces/ref/JC3248W535EN/src/esp_lcd_axs15231b.c:461-496` — touch read protocol:
  I2C @ 0x3B, write `{0xB5,0xAB,0xA5,0x5A,0x00,0x00,len_hi,len_lo,0,0,0}` then read 8
  bytes (`num`, `x=(xh&0xF)<<8|xl`, `y=(yh&0xF)<<8|yl`), 1 point max.
- `/workspaces/ref/JC3248W535EN/docs/GPIO_pinout.jpg` (manufacturer table) — LCD
  **DC(RS) = NC** and RST = NC: the panel is configured **QSPI-only** (AXS15231 datasheet
  v0.5: interface selected by IM[3:0] pins; SPI-4wire mode requires the RS pin,
  `DCX→RS`). 4-wire SPI is therefore **not possible** on this hardware.
- `/workspaces/ref/JC3248W535EN/sdkconfig.defaults:1-15` — 16 MB QIO flash, 8 MB **octal**
  PSRAM (ESP32-S3-WROOM-1 N16R8).
- `app/src/keymap.c:36` — ZMK hard-fails without a `zmk,keymap` node;
  `app/include/zmk/matrix.h:21-42` — `ZMK_KEYMAP_LEN` derives from the `zmk,kscan`
  chosen, so a display-only device still needs a (minimal) kscan.
- `zephyr/drivers/spi/spi_esp32_spim.c:502-518` — Zephyr's ESP32 SPI driver maps
  `SPI_LINES_QUAD` → 4 data lines under `CONFIG_SPI_EXTENDED_MODES` (untested on ESP32 —
  the main bring-up risk of this item).
- `zephyr/include/zephyr/dt-bindings/pinctrl/esp32s3-gpio-sigmap.h:188-215` — S3 GPIO
  sigmap can route all four SPI2 data lines (FSPID0/1/2/3) + SCLK + CS to arbitrary GPIOs.
- `zephyr/modules/lvgl/Kconfig.input` (`LV_Z_POINTER_FROM_CHOSEN_TOUCH`) — a Zephyr input
  touch device + `zephyr,touch` chosen gives LVGL a pointer indev with no ZMK
  display-module changes; `zephyr/modules/lvgl/lvgl.c:400-402` (`LV_Z_AUTO_INIT`) creates
  the LVGL display from `zephyr,display`.
- `app/src/display/main.c:25` — ZMK display module uses `DT_CHOSEN(zephyr_display)`;
  LVGL is 9.5 with the v8 API map (`modules/lib/gui/lvgl/src/lv_api_map_v8.h`).
- `zephyr/soc/espressif/common/Kconfig.spiram:32-43` — `SPIRAM_MODE_OCT` exists for the
  8 MB octal PSRAM; `zephyr/dts/xtensa/espressif/esp32s3/esp32s3_wroom_n16r8.dtsi` and
  `zephyr/dts/vendor/espressif/partitions_0x0_default_16M.dtsi` provide the board memory
  layout.

## Why it matters

- cobantab1a is a product module: it needs a fully working ZMK firmware (BLE HID +
  status display + touch) to be shippable.
- The QSPI display driver and the touch driver are reusable for any future AXS15231B
  module (cobantab1b, …) and are the first non-trivial ESP32 peripheral drivers in this
  fork — they set the pattern for a04's remaining classes (battery, RGB, sensors).
- Doing it as one board + one shield + two drivers keeps the a04 "one peripheral class
  per change set" rule and makes the RAM/current impact measurable.

## Conservative fix

Implementation plan, in phases. Each phase ends in a verifiable gate; hardware gates are
marked _(manual — hardware)_.

### Phase 1 — Board definition (zmk fork)

New board `app/boards/others/cobantab1a/`, modeled on
`app/boards/others/esp32s3_supermini/` (same file set: `Kconfig`, `Kconfig.defconfig`,
`Kconfig.cobantab1a`, `Kconfig.sysbuild`, `board.cmake`, `board.yml`,
`cobantab1a_esp32s3_procpu.dts`, `..._procpu-pinctrl.dtsi`, `..._procpu.yaml`,
`..._procpu_defconfig`, `..._procpu_zmk.dts`, `..._procpu_zmk.zmk.yml`,
`..._procpu_zmk_defconfig`, `support/`):

- `.dts` includes `espressif/esp32s3/esp32s3_wroom_n16r8.dtsi` (16 MB flash + 8 MB octal
  PSRAM) + `espressif/partitions_0x0_default_16M.dtsi`.
- PROCPU-only target (consistent with the existing S3 work), BLE enabled,
  USB-Serial/JTAG console (USB-C wired to the internal PHY, same as the reference),
  `zephyr,bt-hci`, `zephyr,flash`, `zephyr,code-partition` chosen.
- PSRAM enabled as **octal**: `CONFIG_SPIRAM=y`, `CONFIG_SPIRAM_MODE_OCT` (board
  defconfig or the shield conf).
- Pinctrl: UART0 recovery console on GPIO43/44 (kept out of the display wiring).

Gate: `west build -b cobantab1a/esp32s3/procpu/zmk` compiles (first
`--no-blobs`, then with `hal_espressif` blobs); image size sane for 16 MB.

### Phase 2 — AXS15231B QSPI display driver (zmk fork)

New Zephyr `display` driver in `app/module/drivers/display/` (pattern: `il0323.c`):

- `axs15231b.c` + `Kconfig.axs15231b` (rsource from `Kconfig` under `if DISPLAY`),
  `CONFIG_AXS15231B` depends on `SPI` + `SPI_EXTENDED_MODES`.
- Transport: Zephyr SPI API on the panel's SPI controller node:
  - command: `SPI_OP_4WIRE_TX` 32-bit opcode (`0x02<<24 | cmd`) + `SPI_CS_HOLD` →
    `SPI_OP_TX` 8-bit command + `SPI_CS_HOLD` → `SPI_OP_TX` params (CS released);
  - color: `SPI_OP_4WIRE_TX` opcode (`0x32<<24`) + `SPI_CS_HOLD` → `SPI_OP_4WIRE_TX`
    RGB565 buffer (CS released).
  - SPI mode 3 (CPOL=1, CPHA=1), `spi-max-frequency` 40 MHz (reference rate; GPIO-matrix
    routing may cap lower — verify at bring-up; 20 MHz ≈ 32 fps full-frame is acceptable).
- Init: port the reference `esp_bsp.c` init sequence verbatim (the panel-tuned one).
- Reset: `SWRESET` (0x01) — no RST pin; optional `reset-gpios` property for future
  revisions.
- Frame writes: `CASET` (0x2A) + `RAMWR` (0x2C); **no `RASET` in QSPI mode** (per the
  reference driver); RGB565, 320×480.
- Blanking: `blanking_on` = `SLPIN`+`DISPOFF`, `blanking_off` = `SLPOUT`+`DISPON`;
  optional `backlight-pwm` DT property → driver sets PWM duty in the blanking handlers.
- Binding: `app/module/dts/bindings/display/axs,axs15231b.yaml` (SPI child node:
  `width`, `height`, `backlight-pwm`, `reset-gpios`); vendor prefix registered in
  `app/module/dts/bindings/vendor-prefixes.txt` if needed.

Gate: driver compiles for `cobantab1a`; _(manual — hardware)_ panel powers up, accepts
the init sequence, and shows a test pattern (simple test app or log-verified init before
full LVGL integration).

### Phase 3 — Shield + ZMK display integration (zmk-config)

New shield `boards/shields/cobantab1a/` in `/workspaces/zmk-config`:

- `cobantab1a.overlay`:
  - `&spi2` enabled: QSPI pinctrl SCLK→47, D0→21, D1→48, D2→40, D3→39 (D2/D3 via raw
    `ESP32_PINMUX` sigmap macros — the convenience header lacks them), `cs-gpios` 45,
    `dma-enabled`; AXS15231B node (320×480, 40 MHz, backlight-pwm).
  - `&i2c0` enabled (SDA 4 / SCL 8, 400 kHz) with the touch node @ 0x3B.
  - PWM node for the backlight (GPIO 1, 5 kHz, 10-bit duty).
  - chosen: `zephyr,display`, `zephyr,touch`.
  - **1×1 phantom kscan** on an inert GPIO + 1-key keymap bound to `&none`
    (`app/dts/behaviors/none.dtsi`) — required by `app/src/keymap.c:36`. Kept trivially
    replaceable: when physical keys land on a future revision, the kscan/keymap grow in
    the shield overlay only.
- `cobantab1a.conf`: `CONFIG_ZMK_DISPLAY=y`, LVGL 16-bit, VDB ≈ 10–20 % of the screen
  with the LVGL heap in a PSRAM region (`CONFIG_LV_Z_MEMORY_POOL_ZEPHYR_REGION`),
  `CONFIG_SPIRAM`/octal, `CONFIG_SPI_EXTENDED_MODES`, display tick/blanking settings.
- `Kconfig.defconfig` (`ZMK_KEYBOARD_NAME "Coban Tab 1a"`), `Kconfig.shield`,
  `cobantab1a.zmk.yml`.

Gate: _(manual — hardware)_ ZMK status screen (layer/battery/output/WPM widgets) renders
on the panel; BLE pairing + typing works with the display active.

### Phase 4 — AXS15231B touch driver (zmk fork)

New Zephyr `input` touch driver (pattern: `zephyr/drivers/input/input_gt911.c`):

- `app/module/drivers/input/axs15231b_touch.c` + Kconfig + binding
  `app/module/dts/bindings/input/axs,axs15231b-touch.yaml`.
- I2C @ 0x3B; reference read protocol (11-byte command, 8-byte response, 1 point);
  polling via `k_work_delayable` (~10 ms — no INT pin on this hardware); optional
  `interrupt-gpios` / `reset-gpios` properties for future revisions.
- Reports via `input_report_touch()`; `zephyr,touch` chosen → Zephyr LVGL port
  auto-creates the pointer indev (no ZMK display-module changes).

Gate: _(manual — hardware)_ touch position tracks on screen (LVGL pointer; verify with a
debug screen or log before/after).

### Phase 5 — Backlight, idle blanking, validation, CI (both repos)

- Backlight PWM + `CONFIG_ZMK_DISPLAY_BLANK_ON_IDLE` on the idle/sleep activity states.
- a04 acceptance: measure RAM (SRAM + PSRAM) and current with display on/off and with
  touch polling on/off; record in `esp-port/test-results/`; demonstrate no BLE/HID
  interference while the display is active.
- `esp-port/build_cobantab1a.sh` (modeled on `esp-port/build_zmk_s3.sh`); compile target
  in `app/core-coverage.yml`; update `esp-port/README.md` phase table.

## Out of scope (for now)

- Physical keys on cobantab1a (none on this revision; the 1×1 phantom kscan is a
  placeholder — future keys are a shield-only change).
- TE (tearing effect) pin, microSD, I2S speaker, battery ADC — no ZMK equivalents.
- QSPI speed beyond what GPIO-matrix routing allows; a dedicated SPI-HAL QSPI driver is
  the **fallback** only if `spi_esp32_spim` quad mode proves broken at bring-up.
- Battery monitoring / RGB / sensors (separate a04 classes, separate change sets).
- USB HID / Studio-over-USB on cobantab1a (gated by the Phase 8 / b04 USB-PHY work).

## Acceptance / verification

- [x] `west build -b cobantab1a/esp32s3/procpu/zmk -- -DZMK_CONFIG=... -DSHIELD=cobantab1a`
      compiles cleanly (no-blobs and with blobs) and produces a 16 MB-layout image.
      (2026-11-14: full blobs build via `esp-port/build_cobantab1a.sh` — FLASH 566884 B,
      `ext_dram_seg` 1788 KB; LVGL VDB at 0x3C060000 (60 KB) and LVGL heap at 0x3C06F000
      (256 KB), both in PSRAM.)
- [ ] _(manual — hardware)_ Panel initializes (init sequence accepted) and the ZMK
      status screen renders at 320×480 RGB565.
- [ ] _(manual — hardware)_ Touch input moves the LVGL pointer in sync with the finger.
- [ ] _(manual — hardware)_ Backlight PWM works; display blanks on idle and unblanks on
      activity.
- [ ] _(manual — hardware)_ BLE pairing + typing verified with the display active
      (no interference, per a04).
- [ ] RAM (SRAM/PSRAM) and current impact with display+touch on vs off recorded in
      `esp-port/test-results/`.
- [x] No CI entry by decision (coban product shields live in the external config repo,
      which CI cannot access; canonical build path is `esp-port/build_cobantab1a.sh`).
- [x] `backlogs` index updated (item created 2026-11-14); a04 cross-referenced with the
      display class in progress.
