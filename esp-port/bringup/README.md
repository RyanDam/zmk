# AXS15231B bring-up instruments

**Status 2026-11-21: SOLVED.** The panel paints from ZMK (init + fills, GDMA,
both 1 MHz and 40 MHz) and commands act on the glass, with (1) the frame built as
ONE transaction (header in the controller's command+address phases - see findings
§16 root cause 1) and (2) SPI mode 3 emulated the way IDF does, via the GPIO-matrix
clock inverter (`axs_mspi_clock_invert()` - findings §16 root cause 2). These
fragments are the instruments that produced that verdict and the fastest way to
re-verify it on a bench.

Four builds, four questions, one board. Everything here is bring-up scaffolding:
`CONFIG_AXS15231B_*` defaults to n everywhere, and the cleanup checklist in
[`../axs15231b-bringup-findings.md`](../axs15231b-bringup-findings.md) §11 says what
comes out before this can be merged.

## Rules these experiments depend on

- **Power cycling destroys the run.** The panel's GRAM and registers survive a
  reflash and a reset — only a USB unplug clears them. That is the one free
  sensor this hardware gives: a picture that survives our firmware is a picture we
  did not touch. Unplug/replug **only** when a fragment tells you to.
- **`./flash.sh jc3248` first** when a fragment says so: that is the vendor demo,
  the only thing proven to paint this panel. Confirm its UI is on the glass, do not
  power cycle, then flash ours.
- **Throughput is the sanity check, not the word "painted".** A full-panel fill is
  320×480×2 bytes = 2 457 600 bits, so at 1 MHz it cannot take less than 2457.6 ms
  on one line or 614.4 ms on four. Anything far below that sent nothing.
- The instruments run on a looping thread, so a person arriving at any moment
  catches a transition instead of missing a 35 s window.

## Build and flash

From the workspace root (`Firmware/src`):

```sh
sh zmk/esp-port/build_cobantab1a.sh -C zmk/esp-port/bringup/<fragment>.conf   # + -O <overlay> if listed
./flash.sh tab1a                                   # then watch the console
./flash.sh tab1a --no-reset && west monitor -d zmk/build/cobantab1a   # to see boot from the top
```

`-C` is `EXTRA_CONF_FILE` (applied after the shield's conf, so it can switch
instruments off as well as on); `-O` is `EXTRA_DTC_OVERLAY_FILE` (applied after the
shield's overlay, so it wins on the properties it touches).

## The fragments

| Fragment | Also needs | Question it answers | Decisive log line / glass |
| --- | --- | --- | --- |
| `readback.conf` | — | **Does the panel hear us, and can it answer?** Bit-bangs the *complete* vendor init (the old probe skipped the table, which voided its result), and reads `04h/0Ah/0Bh/0Ch/DAh` back before and after every command | `gpio read …: reg 0Bh CHANGED 00h -> 80h` — a register that moves can only have been written by our bits. `no answer` everywhere means the read path itself is unproven |
| `init-only.conf` | vendor demo on the glass first | **Is it the commands that darken the panel rather than the pixels?** Boot sends the panel nothing; every 15 s the init sequence (no reset) is re-sent; nothing writes pixels | The vendor picture fades / darkens / rotates / inverts → commands land, init is the fault → bisect with `CONFIG_AXS15231B_INIT_LIMIT`. Survives untouched → the transport is the fault |
| `skip-init-fill.conf` | vendor demo on the glass first | **Do our pixels land, on a panel already known alive?** No commands at all, then the fills | Our pattern replaces the vendor's → pixel path is fine, only the init darkens it |
| `spi-path.conf` | `spi-path.overlay` | **Is it our MSPI port, or the panel?** Same fills and same visible commands through `&spi2` — the path whose `DISPLAY_OFF` once visibly blanked the panel | Bars paint here but not on MSPI → the product ships on the SPI path and MSPI becomes an optimisation |
| `invert-probe.conf` | vendor demo on the glass first | Do the *visible* commands (MADCTL, inversion) survive our framing? | Ran before the fix: a fade with no inversion - the half-clock signature, read backwards |
| `blank-probe.conf` | `boost-clock.overlay` (or accept 1 MHz) + a live picture to act on | **Do unambiguous commands act, one at a time?** SLPOUT, DISPON, SLEEP_IN, SLPOUT, INVON, INVOFF every 4 s, each logged with the effect it must produce | Post-fix verdict: fade ~8 s, recover ~12 s, flip ~16 s, restore ~20 s, repeating — in step with the log. This is the re-verification instrument |
| `gpio-quad.conf` | vendor demo on the glass first | Can a frame reach the panel with **no controller at all**? Bit-bangs the golden 1-wire header (32 clocks on D0, mode 3) + INV/INVERT/SLEEP commands | The only instrument that bypasses the MSPI block entirely; kept for that |
| `full-init-40mhz.conf` | `boost-clock.overlay` (or drop it: paints at 1 MHz too) | **Can ZMK drive this panel from a virgin power-on?** Full init + fills, no SKIP_INIT — needs an unplug/replug first (findings: power is this panel's reset) | Post-fix verdict: real pictures, DMA and PIO, both frequencies. The go/no-go instrument |

Verdicts of the first four (all run *before* the §16 fixes; every negative is
explained by it): `readback` heard only phantom `0x80`s (half-clock sampling);
`init-only` was inconclusive by design; `spi-path` was never re-run post-fix and
is not needed. The fix lives in the driver, so every fragment builds on it.

Suggested order for a fresh bench check: `full-init-40mhz` (one flash, one
picture, whole path proven) → `blank-probe` (commands, on the live picture).

## Knobs

| Symbol | Use |
| --- | --- |
| `CONFIG_AXS15231B_INIT_LIMIT` (−1…32) | Send only the first N entries of the 32-entry panel table. `0` = just the commands around it. The entries are order-dependent, so bisect by prefix: 32 entries is five builds |
| `CONFIG_AXS15231B_PIXELS_1WIRE` | Pixel data on one data line instead of QIO. A 1-wire fill that paints while a quad one does not localises the fault to the wide phase |
| `CONFIG_AXS15231B_SKIP_INIT` | No panel commands from the boot path — the previous firmware's picture and display state survive |
| `CONFIG_AXS15231B_INIT_ONLY` | Re-send the init sequence on the bring-up thread and write no pixels |
| `CONFIG_AXS15231B_QSPI_READBACK` | Read the panel's own registers, by bit-bang, opcode `03h` and `0Bh` ± a dummy phase, and report any register that *changed* |

While any of `COLOR_TEST` / `FILL_TEST` / `INIT_ONLY` is on, `paint_hold` keeps LVGL
and the blanking hooks off the bus, so the log is the whole conversation with the
panel.
