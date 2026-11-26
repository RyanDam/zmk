# Carried Zephyr patches

`west update` resets `zephyr/` and `modules/` to the pins in `app/west.yml`, so
every patch here is re-applied by [`../apply-zephyr-patches.sh`](../apply-zephyr-patches.sh)
after each update. Each patch ships with a `.verify` sidecar (probe strings that
only the patched tree contains — exit codes from `patch(1)` are not evidence) and
optionally a `.target` sidecar naming the repo it applies to (default `zephyr/`).
`RETIRED` lists patches that must **not** come back because the pin now has the
fix.

This directory is deliberately diff-quiescent (`.gitattributes`: `-diff`), so a
code review never reads 3.8 k lines of vendored hunks. Read the table below
instead.

| Patch | Repo | Why it exists | Owner | Can it go away? |
|---|---|---|---|---|
| `0002-mspi-esp32` | zephyr | ESP32 MSPI (memory-mapped/quad) SPI controller support. The AXS15231B panel runs on MSPI (`[axs15231b.c](../../app/module/drivers/display/axs15231b.c)`) | upstream PRs [#121107](https://github.com/zephyrproject-rtos/zephyr/pull/121107) stacked on [#117188](https://github.com/zephyrproject-rtos/zephyr/pull/117188) | when those merge |
| `0003-hal-espressif-spi-hal-for-mspi` | hal_espressif | the `spi_hal` companion to 0002 | same PRs | same |
| `0004-zephyr-post-boards-shields-extension` | zephyr | adds the `post_boards_shields` cmake hook; without it ZMK's `app/boards/post_boards_shields.cmake` never runs, so the `<shield>.keymap` and `app/dts/behaviors.dtsi` never reach the devicetree — **every ZMK build in this workspace breaks** | fork content (Pete Johanson) | if upstream ever takes the hook |
| `0005-soc-nordic-optional-binding-header-validation` | zephyr | `soc/nordic/validate_binding_headers.c` force-includes every dt-bindings header the DT pulled in; ZMK's `app/include/dt-bindings/zmk/keys.h` defines single-letter HID macros (`A`…`Q`) that collide with CMSIS → **the nRF pads do not build without it** | fork content | if upstream makes validation opt-out |
| `0006-usbd-cdc-acm-suspend-resume-and-irq-loop` | zephyr | four CDC-ACM fixes (suspend/resume + an IRQ loop) re-authored against the 4.5 `usbd_cdc_acm.c` rewrite; replaces two fork commits that do not cherry-pick | **ours** (2026-11-19) | upstream candidate — file together with the DWC2 TxFIFO report, [`../../../../docs/wave2-implementation-report.md`](../../../../docs/wave2-implementation-report.md) §9.5 |
| `0007-boards-boardsource-blok-rp2040` | zephyr | the BoardSource blok board is absent from stock 4.5 while `app/boards/boardsource/blok/` carries ZMK overlays for it | fork content (board parity) | when the ZMK overlays are dropped or the board is upstream |
| `0008-soc-arm-stm32-rom-bootloader` | zephyr | `st,stm32-bootloader` binding + f0/f4/wb dtsi work that fork board defs rely on | fork content (board parity) | same |
| `0009-retention-skip-mutex-pre-kernel` | zephyr | retention must not take a mutex before the kernel starts; the pads boot with `CONFIG_RETENTION=y` + `CONFIG_RETAINED_MEM_MUTEXES=y` | fork content | if upstream merges it |
| `0010-boards-seeed-xiao-ble-uf2-runner` | zephyr | `xiao_ble` UF2 runner default (board exists upstream; the runner choice does not) and `app/boards/seeed/xiao_ble/` uses it | fork content (board parity) | same |
| `0011-drivers-retained-mem-bbram` | zephyr | BBRAM `retained_mem` driver + `zephyr,retained-bbram` binding, used by [`ferris_stm32f072xb_zmk.dts:160`](../../app/boards/pierrechevalier83/ferris/ferris_stm32f072xb_zmk.dts) | fork content (third party) | if upstream takes it |
| `0012-pm-restore-always-defined-slots` | zephyr | carry of the revert of "pm: only define slots if CONFIG_PM is enabled"; all three products build `CONFIG_PM_DEVICE=y` | fork content | when the revert is resolved upstream |

## Do not "clean up" by dropping a row

Each of 0004, 0005, 0007, 0008, 0010, 0011 exists because **this fork's board
tree still carries the boards/headers it touches** — they are not tab1a files,
but a build of `nice_nano` (pads) or any of those boards fails without them.
Before removing one: delete nothing here, instead try one build of `pad12b` plus
the affected board with the patch unapplied (`git -C zephyr checkout -- <files>
in the patch>`) and let `apply-zephyr-patches.sh`'s verify step be the judge.
Real retirements go into `RETIRED` with the commit that fixed it upstream, the
way `0001-spi-esp32-fix-quad-line-mode-truncated-to-16-bit.patch` did (fixed
upstream by `24cc93286`, retired with the v4.5.0 pin).
