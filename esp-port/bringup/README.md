# tab1a bench instruments

Build fragments for on-glass work on the Coban Tab 1a: what to flash to see a
thing. Everything here is **bench-only** — none of it is in the product build
(`zmk-config/boards/shields/cobantab1a/`), and none of it compiles extra code
into the product image.

For panel-driver instruments (fill/command/blank/readback probes, the old
`CONFIG_AXS15231B_*` symbols) see
[`../axs15231b-bringup-findings.md`](../axs15231b-bringup-findings.md) §16 and
commit `cd01099` (`git show cd01099 -- esp-port/bringup app/module/drivers/display`)
— the panel now ships production code only.

## Live — open backlog items

Build with `sh zmk/esp-port/build_cobantab1a.sh [-C <conf>] [-O <overlay>]` from
the workspace root, then `./flash.sh tab1a`. Wiring + expectations:
[`../tab1a-bench-runbook.md`](../tab1a-bench-runbook.md).

| Item | File(s) | What it is |
|---|---|---|
| b06 / a14 bug #3 (RPC over the CDC does not answer) | `tab1a-usb-1cdc.overlay` | the known-good daily shape: HID + one Studio-RPC CDC, console/shell on UART0 |
| | `tab1a-usb-1cdc-usj.overlay` | same, console/shell on USB-Serial/JTAG instead (dark by design once the OTG hand-off runs — use for enumeration only) |
| | `tab1a-usb-uart0-console.overlay`, `tab1a-usb-usj-console.overlay` | console-pinning variants used by the bisection that closed bug #2; still the only way to see boot logs on a USB build |
| Studio-RPC connect freeze (a14, b06) — **closed 2026-11-27** | *(nothing live — the instruments moved to `archive/` below)* | **Root cause: the `usbd` thread ran out of stack.** Zephyr's `CONFIG_USBD_THREAD_STACK_SIZE=1024` default is exceeded on this board (peak 1408 B — DWC2 UDC + CDC-ACM class, with `LOG_MODE_IMMEDIATE` formatting on the caller's stack), and the overrun wrote an `-ENOMEM` over the adjacent `bt_task_handle` thread object, so ~1 s after a host write a context switch into `btController` died in `xtensa_restore_high_regs`: `EXCCAUSE 28 (load prohibited)`, `VADDR 0`, `A2 = 0xfffffff4`. That is why it read as a USB/RPC bug: making the app's `serial_cb` a literal no-op changed nothing. **Fix, both now product settings in `zmk-config`'s `cobantab1a.conf`:** `CONFIG_USBD_THREAD_STACK_SIZE=2048` (1024 faults at *any* `UDC_DWC2_STACK_SIZE`, 8192 tried; 2048 and 4096 clean — selftest + 14-rung ladder + 25 heartbeat beats, 640 B spare at the peak) and `CONFIG_RESET_ON_FATAL_ERROR=y` (a fault then drops the bus and re-enumerates instead of halting with the CDC still answering a connect — which is exactly what made the symptom "frozen screen, port alive"). Survives with Bluetooth off too, so the BT controller was only the victim |
| c03 (S3 live-BLE wedge) | `tab1a-ble-noconsole.conf` | minimal self-typing repro (`CONFIG_ZMK_VKEY_BENCH`, no console) — wedges ~10 s after a central reconnects |
| | `tab1a-ble-rlfix.conf` | same + console/link logging: capture the mode-(b) last words |
| | `tab1a-ble-smpoff.conf` + `a19-smpoff-experiment.patch` | A19 open-link build (SMP off; phones cannot pair by design). Patch is a **source hack — never commit it**, `zephyr/` stays clean |
| | `tab1a-display-only.conf` | display ON / BLE OFF arm of the a13 crash bisection (2026-11-23) |
| | `c3-ble-bench.conf` | the ESP32-C3 CONTROL build (survives → the bug is S3-side) |
| | `c3-central/` + `c3-central-capture.py` | open-link BLE central for esp32c3_supermini; built, never run — c03 next-step #1 |
| | `patches/hci_esp32-rl-emul-and-recv-wait.patch` | the two zephyr HCI experiments, re-appliable on demand |
| | `logs/` | preserved death evidence (`dumpall.log` = mode-(b) last words, `repro-v3.log` = tap-5 burst death) |
| c03 write-up | `a13-upstream-report-draft.md` | near-complete upstream report, ready to file |
| h03 (I2S audio) | `tab1a-devkit.overlay` + `tab1a-devkit.conf` | final-board peripheral harness (4-key matrix, WS2812 chase, I2S tone, VBAT ADC); probes in `app/src/tab1a/`. Validated driver-side 2026-11-21 (h01 closed) — it is now the vehicle for h03/h04 |
| | `tab1a-devkit-i2sdbg.conf` | same overlay with `I2S`/`DMA` at DBG (the `trigger(START) → -EIO` trap, see runbook) |
| h05 (touch INT) | `tab1a-touchdbg.conf` | puts the on-glass touch readout back (`CONFIG_ZMK_DISPLAY_DEBUG_OVERLAY_TOUCH`): raw x/y + swipe verdict chip, plus a dot-under-finger page with the controller's watchdog counters |

## Archived — backing item closed

`archive/` keeps the recipe, not the workflow. Each file's header says which
item it belongs to and where the evidence landed.

| Item (status) | File | Why it is here |
|---|---|---|
| c02 (DONE 2026-11-21) | `archive/c02-ble-repro.conf`, `archive/c02-ble-repro-v3.conf`, `archive/c02-storage-only.conf` | repro + qualification builds for BLE/storage; the findings moved to `esp-port/README.md` and c03 |
| panel bring-up (solved) | `archive/boost-clock.overlay` | frequency was exonerated (1 MHz and 40 MHz paint alike) |
| a14/b06 crash instruments (closed 2026-11-27) | `archive/tab1a-crash-smuggle.conf` | the fault smuggler that found the stack overflow: fault text kept in `.rtc_noinit` and re-emitted unprompted as `#CRASH1` chunks on the RPC port, with `#SMUGGLE1` reset counters and a `#STK` stack high-water table. The module it enabled was removed with the investigation; the header keeps the knob recipe plus the two traps it exposed — `__noinit` does not survive this board's reset (`.rtc_noinit` does), and the reboot lives inside `k_sys_fatal_error_handler`, so an override must call `sys_reboot()` itself |
| | `archive/tab1a-crash-forensics.conf` | stack sentinel + periodic thread analyzer: **dead end**, each one makes the tab fault ~1 s into boot, so it is not a usable repro vehicle |
| console over the RPC CDC (tried, rejected) | `archive/tab1a-console-over-usb.conf` + `.overlay` | negative result kept so nobody retries it: the composite enumerates but macOS binds no CDC interface, so there is no `/dev/cu` node to read. The tab's console stays on UART0/P1 |

## The one rule these experiments ran on

The panel has no reset line, so **a USB unplug is its only reset**: a picture
that survives a reflash is a picture the new firmware never touched, and a
polluted panel (stray sleep commands) misbehaves even under good commands.
