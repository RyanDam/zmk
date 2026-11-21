# AXS15231B bring-up instruments — retired

The panel works (see [`../axs15231b-bringup-findings.md`](../axs15231b-bringup-findings.md)
§16: frame as ONE transaction with the header in the controller's command/address
phases, and SPI mode 3 reached through the GPIO-matrix clock inverter). The
driver now ships production code only, so every `CONFIG_AXS15231B_*` instrument
symbol is gone, and with it the fragment files that enabled them.

**The full instrument set — fragments, probes, fill/command/blank/readback tests
— is preserved in commit `cd01099`** (`git show cd01099 -- esp-port/bringup
app/module/drivers/display`), together with the findings document as it stood at
the solve. If this panel ever misbehaves again, start there to re-build the
instruments, and re-read §16 first: both real defects (frame splitting, and the
mode-3 waveform on a GPIO-matrix pad) produce symptoms that look like everything
else in the ledger.

Still useful without any instrument code:

- `boost-clock.overlay` — raises the panel clock to 40 MHz to check
  throughput-sensitive behaviour (the panel paints at the shield's stock 1 MHz
  and at 40 MHz alike; frequency was exonerated either way).

    sh zmk/esp-port/build_cobantab1a.sh --no-blobs -O zmk/esp-port/bringup/boost-clock.overlay

Remember the one rule these experiments ran on: the panel has no reset line, so
**a USB unplug is its only reset** — a picture surviving a reflash is a picture
the new firmware did not touch, and a polluted panel (stray sleep commands)
misbehaves even under good commands.
