# Tab 1a Studio RPC harness (backlog a14)

Scripted ZMK Studio RPC client for the Coban Tab 1a bench path — the
always-available proof that the screen/theme/stats/pomo/assets
subsystems round-trip, with no GUI required.

What makes it drift-proof:

- **Framing** (`src/framing.js`) is a port of the firmware's
  `app/src/studio/msg_framing.c` (SOF/ESC/EOF byte stuffing), unit-tested
  against the reference client's vectors (`npm run framing-test`).
- **Protobufs** (`src/protos.js`) are loaded **at runtime** from the same
  west module the firmware codegens against
  (`modules/msgs/zmk-studio-messages/proto/zmk`) — there is no generated
  copy to go stale, and no build step.
- **RPC semantics** (`src/rpc.js`) mirror the reference client's
  `call_rpc`: one in-flight request, request-id matching,
  `meta.simpleError` → rejected promise, notifications on a side tap.

## Install

```sh
cd zmk/app/scripts/studio-harness
npm install          # serialport + protobufjs (Node >= 20)
npm run framing-test
```

## Bench wiring

UART0 on the P1 header (see `../../../esp-port/tab1a-bench-runbook.md`
for the authoritative steps):

| tab1a P1 | USB-TTL adapter (3.3 V logic!) |
|---|---|
| GPIO43 (UART0 TX) | RX |
| GPIO44 (UART0 RX) | TX |
| GND | GND |

115200 8N1. The device console/shell is on the USB-Serial/JTAG port —
keep that connected too for the firmware log while the harness talks.

**b06 (merged):** on the default `tab1a` build (since 2026-11-26) the same
harness talks over the USB-C port's CDC-ACM device instead —
`--port /dev/cu.usbmodemXXXX` (the "ZMK Studio RPC" CDC interface of the
HID composite); the console lives on the P1 UART0 pins. Full walk:
`esp-port/tab1a-bench-runbook.md` §b06.

## Use

```sh
node src/cli.js --port /dev/cu.usbserial-XXXX info
node src/cli.js --port /dev/cu.usbserial-XXXX lock          # unlock for SECURED calls
node src/cli.js --port ... screens
node src/cli.js --port ... screen stats
node src/cli.js --port ... time --sync
node src/cli.js --port ... theme get
node src/cli.js --port ... stats config 3 1 4 3 2           # 2x4 layout, CPU/GPU/RAM/CPU-T
node src/cli.js --port ... stats push --demo --hz 2 --for 30
node src/cli.js --port ... pomo set 2 120 60 0 && node src/cli.js --port ... pomo start
node src/cli.js --port ... assets list
node src/cli.js --port ... assets upload ~/art/meadow.gif --fps 12   # a15 chunked+CRC
node src/cli.js --port ... assets status        # resume point of a live session
node src/cli.js --port ... assets delete meadow && node src/cli.js --port ... assets gc
node src/cli.js --port ... keymap rebind 0 4 <behaviorId>   # stock keymap RPC
node src/cli.js --port ... macro rename 0 "MAIL" && ... macro save
node src/cli.js --port ... selftest                         # the whole acceptance walk
```

`selftest` runs the a14 acceptance sequence (navigate + event, time sync
shows synced, theme live + event, stats frame, pomo config/start/state
event, assets list/activate) plus the a15 upload trio (64 KiB upload →
list + generation bump → activate live; a deliberately corrupted stream
refused with `committed=false`; delete → built-in fallback reasserts),
keymap rebind round-trip, macro rename round-trip; prints PASS/FAIL per
step, and flags the few steps that need an on-glass owner look. Save
the transcript to `esp-port/test-results/` as the evidence.

## Notes

- SECURED handlers (theme set, stats config, pomo control, keymap,
  assets activate) need `lock` first — every non-`--help` invocation is
  short-lived, so run `lock` right before a batch or `selftest` does it.
- The device auto-locks after `CONFIG_ZMK_STUDIO_LOCK_IDLE_TIMEOUT_SEC`
  (default 600 s) of RPC idle.
- BLE stays up but is irrelevant here: `CONFIG_ZMK_STUDIO_RPC_TRANSPORT_UART_PREFERRED`
  pins the RPC to this UART.
