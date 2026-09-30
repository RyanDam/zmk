# a02 — Phase 3: Migrate legacy USB device/HID stack

- **Category:** a (Architecture & maintainability)
- **Severity:** High
- **Status:** DONE
- **Effort (est):** L

## Problem
ZMK's USB HID is built on Zephyr's legacy USB device stack (`CONFIG_USB_DEVICE_STACK`,
`usb_enable()`, legacy descriptor macros, `usb_hid_register_device()`, `hid_int_ep_write()`),
all marked `DEPRECATED` in Zephyr 4.4.1. The ESP32-S3 OTG/DWC2 platform is only reachable
through the modern UDC/USBD stack (`zephyr_udc0`), so S3 wired HID is impossible on the
legacy stack.

## Evidence
- `app/src/usb.c`, `app/src/usb_hid.c` — legacy stack enablement, HID registration, and report submission.
- `app/Kconfig` — selects `CONFIG_USB_DEVICE_STACK` for ZMK USB output.
- `app/src/endpoints.c`, `app/src/activity.c`, `app/src/events/usb_conn_state_changed.c`, `app/src/studio/` — consumers of USB connection state, activity, and Studio UART/CDC.
- `report.md:48-61` — legacy vs modern USB device stack comparison and affected ZMK core.
- `implement_plan.md:166-203` — Phase 3 tasks and exit gate.
- `zephyr-4.4.1-upgrade-notes.md` ("The remaining 99 warnings, in detail") — the 4.4.1
  coban builds currently emit 96 warnings that exist solely because of the legacy stack:
  77 `-Wdeprecated-declarations` function warnings, 19 `USB_TRANS_READ/WRITE/NO_ZLP`
  `__DEPRECATED_MACRO` warnings (from Zephyr's own legacy stack files), and 2 "Deprecated
  symbol USB_DEVICE_STACK/USB_DEVICE_DRIVER" Kconfig warnings. All vanish when the legacy
  stack stops compiling.

## Why it matters
- The S3 DWC2 path is a UDC-only model; keeping the legacy stack would make the new S3 USB
  target depend on deprecated, no-longer-focused-on upstream code. It is a lifecycle/model
  change (explicit `usbd_context`, `usbd_msg_register_cb()` lifecycle), not a renamed API.

## Conservative fix
- Inventory all legacy USB references and all user-facing USB functions: keyboard/consumer/mouse reports, boot protocol, endpoint switching, connection status, power/activity state, display status widgets, USB logging, and Studio UART transports.
- Design a small ZMK USB abstraction that keeps endpoint/report callers independent of the Zephyr USB stack API; avoid a broad rewrite of HID report generation.
- Implement an explicit USBD context bound to `zephyr_udc0`: descriptors, speed-specific configurations, and class registration attached before `usbd_init()`/`usbd_enable()`; lifecycle/VBUS/configuration notifications via `usbd_msg_register_cb()`.
- Implement the modern HID report submission path for keyboard, consumer, and mouse reports; port protocol/idle callbacks, suspend/resume, remote wakeup, and connection-state event propagation.
- Port optional USB CDC/Studio/logging configurations separately from HID so disabling them does not break basic keyboards.
- Update board defaults and Kconfig dependencies from the legacy stack to the new USB device/controller model.
- Test on controllers already known to work in ZMK before introducing S3: at minimum one Nordic, one RP2040, and one STM32 USB board.

## Out of scope (for now)
- Enabling the S3 DWC2 controller on the SuperMini (b04) — that is gated separately on the USB-PHY/eFuse decision.

## Acceptance / verification
- [x] Legacy USB stack symbols/APIs no longer underpin supported ZMK USB HID.
      (Sweep: no `USB_DEVICE_STACK` (non-NEXT), `USB_DEVICE_HID`,
      `USB_UART_CONSOLE`, `USB_HID_BOOT_PROTOCOL`, `usb_dc_*` APIs, or
      legacy `usb_device.h`/`class/usb_hid.h` includes left in the app tree —
      except `app/boards/moergo/glove80/usb_serial_number.c`, whose
      template-based serial number is inert on the new stack; see the
      upgrade-notes a02 section for the follow-up note.)
- [ ] USB enumeration and report delivery verified on Linux, macOS, and Windows.
      (Verified on **macOS** only: HID keyboard + Studio RPC enumerate and
      deliver. Linux/Windows pending — noted as a known gap at close.)
- [ ] Keyboard boot/report protocol switching, consumer and mouse reports, suspend/resume, unplug/replug, and reset-while-connected all pass.
      (Basic keyboard use + Studio RPC verified on macOS, incl. 200×
      suspend/resume stress cycles. Full protocol suite — boot/report
      switching, unplug/replug, reset-while-connected — pending; noted as
      a known gap at close.)
- [x] Existing USB logging and Studio configurations work where supported.
      (Hardware-verified on macOS 2026-09-30: console CDC-ACM log stream
      clean, Studio connects and stays connected, stress test 200/200.)
- [x] Flash/RAM size comparison against the pre-migration baseline documented,
      with material regressions called out. (cobanpad16a, 4.4.1 legacy-stack
      baseline: FLASH 310168 → 320496 B (+10328 B / +3.3 %), RAM 109236 →
      108396 B (−840 B). Full matrix in `zephyr-4.4.1-upgrade-notes.md`.)
- [x] The 96 legacy-stack deprecation warnings are gone from the coban builds
      (see `zephyr-4.4.1-upgrade-notes.md`). (Verified: pristine cobanpad16a
      and cobanpad12b builds emit **0** `warning:` lines.)

## Status report — 2026-09-28 (work parked on `feat/usb-device-next`)

The Phase 3 work was moved off `feat/esp32` to the dedicated branch
`feat/usb-device-next` as a single amended commit `a81ee1fa` "Remove legacy
USB stack" (original `464ded6a` content with the unverified Studio fixes
amended in); `feat/esp32` was reset to `origin/feat/esp32` ("Close phase 2").

### Done
- Full legacy → `device_next` USB migration (`464ded6a`): explicit
  `usbd_context` on `zephyr_udc0`, descriptor/config/class registration,
  `usbd_msg_register_cb()` lifecycle, HID report submission, boot protocol,
  suspend/resume, remote wakeup, connection-state event propagation.
- Build-verified on cobanpad16a/cobanpad12b: 0 deprecation warnings;
  flash/RAM delta documented (acceptance items 1, 5, 6).
- Zephyr fork fixes (`/workspaces/zmk/zephyr`, branch `v4.4.1+zmk-fixes`,
  still UNCOMMITTED — `app/west.yml` pin not updated):
  - `usbd_cdc_acm.c`: ZLP buffer flag for MPS-multiple last chunk
    (upstream #82151); `usbd_cdc_acm_resumed()` re-triggers pending TX;
    `usbd_cdc_acm_suspended()` + suspended TX path issue a remote wakeup
    when TX data is pending (implements the driver FIXME).
  - `udc_nrf.c`: temporary INF per-endpoint TX enqueue/complete diagnostics
    (skips console ep 0x85 to avoid a log feedback loop) — strip before final.
- App-side safety nets (amended into `a81ee1fa` on `feat/usb-device-next`):
  - `rpc_tx_buffer_write()`: bounded 500 ms drain wait instead of infinite
    spin (fixes deadlock of `studio_rpc_thread` + zmk event thread when the
    TX ring is full).
  - `send_response()`: emits the EOF framing byte even on encode failure.
  - `uart_rpc_transport.c`: RX re-arm listener on `ZMK_USB_CONN_HID` (RX
    side of the same CDC-ACM re-trigger bug).
  - `studio-rpc-usb-uart.conf`: `CONFIG_ZMK_USB_REMOTE_WAKEUP=y`.
  - Temporary `LOG_INF` diagnostics in `rpc.c` / `uart_rpc_transport.c` —
    strip before final.

### Not done
- Acceptance item 2: enumeration/report delivery on Linux, macOS, Windows.
- Acceptance item 3: boot protocol switching, consumer/mouse reports,
  suspend/resume, unplug/replug, reset-while-connected.
- Acceptance item 4: Studio over USB CDC-ACM — **blocked** (below).
- Zephyr fork fixes uncommitted; `app/west.yml` pin not updated.
- Temporary diagnostics not yet stripped (app + fork).

### Current issue: Studio RPC over USB CDC-ACM times out
Symptom: Studio's request is decoded on-device and the response is
generated, but Studio times out; the response bytes only reach the host
after unrelated activity (a touchpad touch), ~20–40 s later, consistently.

Established with UDC-level diagnostics (INF, per-endpoint):
- Endpoint map (usbd auto-assignment in registration order): `0x81` HID IN;
  `0x82`/`0x83`/`0x01` = CDC-ACM instance 0 (notification IN / bulk IN /
  bulk OUT) = **Studio RPC**; `0x84`/`0x85`/`0x02` = instance 1 = console.
- RPC bulk IN (`0x83`) transfers **complete promptly (~150 µs)** right after
  the request is decoded → the host's USB stack IS reading the endpoint and
  the bytes reach the host kernel. The device TX path (app ring → CDC-ACM
  ring → UDC → wire) is healthy.
- No SUSPEND/RESUME events after boot → the suspend/resume + remote-wakeup
  fixes are not the (sole) cause.
- Conclusion: the fault lies between "host kernel has the bytes" and "Studio
  processes them" — either (a) byte-stream corruption / malformed framing,
  or (b) host/Studio-side handling.
- **A/B confirmed (2026-09-28):** the `feat/esp32` build (legacy stack,
  `4d9f2a9b`, original app code) works normally with Studio on pad16a → the
  regression is specific to the `device_next` migration (USB descriptors,
  CDC-ACM class control handling, or ZLP/transfer semantics), not the app
  framing or the host/Studio.
- **Studio client capture (2026-09-29, `tmp/studio_log.log`):** with the
  instrumented client, the **first** RPC after connect (behaviors list)
  round-trips in ~9 ms through every pipeline layer (ENCODER → DECODER
  chunk/frame → DECODED → RESPONSE branch → call_rpc). The **second** RPC
  (another behaviors call, the next "load device info" step) gets **no
  bytes back at all** — no `DECODER chunk`, no decoder error — until the
  app's own 5 s timeout fires. Key discovery: the app's transport label is
  `usb-uart` and the timeout stack trace is `usb-uart-transport.ts` — a
  custom transport **inside the Studio app**, not the library's Web Serial
  transport (the `RAW TX/RX` hooks never fired). That app transport is the
  only uninstrumented layer. `TRANSPORT TX/RX` logging was added at the
  transport boundary in `create_rpc_connection` (commit `66dc235` on
  `diag/new-usb`) to cover whatever transport the app provides.
- **Second capture (2026-09-29, `tmp/studio_log2.log`), with
  `TRANSPORT TX/RX` active:** the failure is **intermittent and
  stateful** — this time even the *first* request (behaviors) got no
  response. `TRANSPORT TX` confirms the library handed the framed bytes
  (SOF / payload / EOF) to the app's transport; **`TRANSPORT RX` is
  completely empty** — no responses, no notifications, across three
  retried requests over ~25 s. So the stall is either (a) app transport
  TX → device (requests never arrive) or (b) device → app transport RX
  (responses never delivered). The device console (UDC per-endpoint
  diagnostics + `rpc.c` INF) is the fork in the road: no `RPC request
  decoded` → (a); decoded + `TX enqueue/complete ep 0x83` but no
  `TRANSPORT RX` → (b). Note: the device was not rebooted between the two
  runs — only the browser reconnected (Vite HMR page load) — so the
  device may enter a run with stale state left by the previous session
  (stuck `TX_FIFO_BUSY`, lost bulk-OUT re-arm, net_buf leak).
- **Raw host test — DECISIVE (2026-09-29).** Host is a **Mac**; the ZMK
  CDC-ACM ports are `/dev/{cu,tty}.usbmodem834402` (RPC) and
  `…834404` (console). `tmp/raw_rpc_test.py` opens the RPC port directly
  (bypassing Chrome/Web Serial), sends Studio's exact first request
  (`ab 22 02 08 01 ad`), and the device **answered with the correct
  66-byte response**. ⇒ The device + macOS `cdc_acm` stack are **healthy**;
  the Studio timeout is a **Chrome / Web Serial / app-transport** problem,
  NOT the firmware. This re-frames the A/B result: the legacy build
  "working" in Studio is not because the device_next *device* is broken —
  the difference must surface specifically through Chrome (port selection,
  the app transport's read/write loop, or a descriptor/control-request
  difference Chrome is sensitive to). Remaining suspects, in order:
  (A) the app connects to the **wrong port** (esp. after an HMR reload —
  testable with `lsof | grep usbmodem` while Studio is connected: RPC is
  `…834402`); (B) a bug in the app's `usb-uart-transport.ts` read/write
  loop (source needed — local to the user's Mac, not on GitHub);
  (C) a Chrome Web Serial quirk vs a device_next descriptor/control
  difference (descriptor diff as fallback).

### Root cause identified — 2026-09-29 (device RX re-arm ratchet)

App source reviewed (Studio checkout cloned to `/workspaces/ref/Connect`):
`frontend/src/app/protocols/usb-uart-transport.ts` is **clean** — it opens
the port (`baudRate: 115200`), pipes `port.readable/writable` straight into
`create_rpc_connection`, and wraps `call_rpc` in a 5 s timeout that cannot
cancel the in-flight read (its own comment says so; the 10 s diagnostic
read timeout in the instrumented client releases the mutex). `lsof`
confirmed Studio holds the correct port (`…834402` = RPC). The client-side
`tee()` backpressure theory was **empirically ruled out** (Node 20 repro:
an unread tee branch does not stall the source — 50/50 chunks flowed).

**The fault is in the device_next CDC-ACM RX re-arm path
(`zephyr/subsys/usb/device_next/class/usbd_cdc_acm.c`):**
- `cdc_acm_rx_fifo_handler()` is a **one-shot** re-arm of the bulk-OUT
  endpoint. **Every bail path returns without rescheduling**: not-enabled/
  suspended (line 729), RX-ring throttle (735), `RX_FIFO_BUSY` already set
  (740), and — the killers — `cdc_acm_buf_alloc()` returning NULL (746,
  **silent**) and `usbd_ep_enqueue()` failing (754).
- The alloc/enqueue failures leave `CDC_ACM_RX_FIFO_BUSY` **set** (it is
  set by `test_and_set` at line 740 *before* the alloc; only the
  ep_request callback clears it, on lines 288/312 — i.e. only when an
  actual transfer completes). Grep-verified: no other code touches the bit.
  ⇒ a failed re-arm = **permanent RX death until a full reboot**.
  `usbd_cdc_acm_disable/enable` (replug) does not clear it, so even a
  re-plug does not recover — only power-cycle does.
- The suspended-bail self-heals only via a later trigger: RESUME →
  `usbd_core` → class `resumed` + `USBD_MSG_RESUME` → `usb_msg_cb`
  (`app/src/usb.c`) → `zmk_usb_conn_state_changed(HID)` →
  `uart_rpc_listener` (`uart_rpc_transport.c`) → `cdc_acm_irq_rx_enable` —
  but that call is **gated on `!RX_FIFO_BUSY`** (line 792), so a stuck bit
  blocks every recovery attempt. `usbd_cdc_acm_resumed()` itself has an
  explicit TODO: it re-triggers TX but **not RX** (line 715).
- The USB bus suspends ~3 ms after idle, so suspend/resume cycles (and the
  suspend-bail window) occur constantly during Studio use.

Consistency with all evidence: run 1 (request #0 OK, then RX dies on a
failed re-arm), run 2 (no reboot — stuck BUSY bit blocks the constant
resume-triggered recovery ⇒ total silence), raw test (works after the
device recovered — **open question: was it power-cycled, not just
re-plugged?**), legacy firmware (old driver re-arms differently ⇒ works).

**Fix — IMPLEMENTED in the zephyr fork (uncommitted, pending review;
build-verified 2026-09-29 via `bash app/build_coban.sh`: 570/570, 0
warnings, FLASH 321708 B / RAM 108444 B, `zmk-16a.uf2` written):**
1. `cdc_acm_rx_fifo_handler()`: on alloc failure → `LOG_ERR` + clear
   `RX_FIFO_BUSY` (next trigger re-arms); on enqueue failure → also clear
   `RX_FIFO_BUSY` (was missing, unlike the TX path). The alloc failure is
   also visible via the pre-existing `udc_ep_buf_alloc` LOG_ERR in
   `udc_common.c` ("Failed to allocate net_buf 64, ep 0x01") — that module
   is NOT silenced by the CDC-ACM console recursion guard
   (`CONFIG_UDC_DRIVER_LOG_LEVEL=INF`), whereas the class module is forced
   to `LOG_LEVEL_NONE` while the console is CDC-ACM.
2. `usbd_cdc_acm_resumed()`: re-arm RX (resolves the line-715 TODO) — if
   `IRQ_RX_ENABLED && !RX_FIFO_BUSY`, submit `rx_fifo_work`. Makes the
   class self-sufficient on resume instead of relying on the app listener.
3. Keep the app-level `uart_rpc_listener` (harmless, idempotent).
4. `udc_nrf.c` temporary diagnostics extended: `RX enqueue ep 0xNN` (bulk-
   OUT re-arm heartbeat — 0x01 RPC, 0x02 console; when it stops for 0x01
   the device RX is dead) and `RX complete ep 0xNN` (a host→device
   transfer landed). No feedback loop: log output goes to console TX 0x85,
   which is still skipped.

**Verification plan:** `tmp/stress_rpc_test.py` (created) hammers the RPC
port with N framed requests, each gap > 3 ms (forces a suspend/resume
cycle per request). 0 failures over N ⇒ RX survives; first failure at k
with all later requests silent ⇒ ratchet reproduced — capture the device
console around k and look for `Failed to allocate net_buf` / `Failed to
enqueue net_buf` / `not enabled or suspended` / `RX buffer to small`.
With the fixed build, the console shows an `RX enqueue ep 0x01` /
`RX complete ep 0x01` heartbeat per request — the heartbeat stopping means
RX died; the resume re-arm (fix #2) should restart it.
Sequence: (optionally) stress the current firmware to reproduce, then
flash the fixed build and re-stress (expect 0 failures), then a final
Studio session.

Client-side findings (`zmk-studio-ts-client`):
- `src/transport/serial.ts:7` opens the port at `baudRate: 12500` (virtual
  for CDC-ACM; should be inert).
- `src/framing.ts` decoder is all-or-nothing: any byte ≠ `0xAB` in the IDLE
  state, or a `0xAB` mid-frame, calls `controller.error()` and kills the
  whole readable stream until reconnect — one stray or partial frame kills
  the session.
- `src/index.ts:103` `call_rpc` has no read timeout (the 10 s diagnostic
  timeout in `diag/new-usb` releases the mutex — verified working in
  run 2: three retried requests each got a fresh `TRANSPORT TX`).
- `tee()` backpressure: **ruled out empirically** (Node 20: an unread tee
  branch does not stall the source; it only accumulates).

Ruled out: endpoint shortage, UDC XFER-event loss, stuck `TX_FIFO_BUSY`
(self-heals on completion), host-side ZLP holding, CDC-ACM class TX stall
(UDC completes promptly), wrong-port selection (lsof), app transport
read/write loop (source reviewed), client `tee()` stall (Node repro).
Net-buf pool exhaustion is **no longer ruled out** — it is the silent
alloc-failure path of the RX re-arm (16×1024 shared `udc_ep_pool`;
steady-state hold ~6–8 buffers, so a leak or a reconfigure-window enqueue
failure are the candidate triggers; the console capture will show which).

### Potential ways forward (updated 2026-09-29)
1. **Reproduce with `tmp/stress_rpc_test.py`** (Studio closed): hammer the
   RPC port with N requests, each gap > 3 ms (one suspend/resume cycle per
   request). If the ratchet reproduces, capture the device console around
   the first failure — the bail log lines (`Failed to allocate net_buf` /
   `Failed to enqueue net_buf` / `not enabled or suspended` / `RX buffer
   to small`) identify the exact trigger path.
2. **Implement the RX re-arm fix** in the zephyr fork `usbd_cdc_acm.c`
   (see "Fix plan" above): clear `RX_FIFO_BUSY` on alloc/enqueue failure +
   log the silent alloc failure; re-arm RX in `usbd_cdc_acm_resumed()`
   (resolve the line-715 TODO). Uncommitted, for review.
3. **Verify:** flash the fixed build (`bash app/build_coban.sh`), re-run
   the stress test (expect 0 failures over N ≥ 500), then a full Studio
   session (the original symptom should be gone).
4. **Cleanup before final.** Strip temporary diagnostics (app `rpc.c`,
   `uart_rpc_transport.c`; fork `udc_nrf.c`, leftover `LOG_INF`s in
   `usbd_cdc_acm.c`), commit the fork fixes and bump the `app/west.yml`
   pin, re-verify with `bash app/build_coban.sh`.

### Diagnostic notes (for whoever picks this up)
- **Build/flash for hardware validation:** `bash app/build_coban.sh` (repo
  root) → `/workspaces/zmk-config/zmk-16a.uf2`. The last flashed build
  (2026-09-28 14:16) still contains the temporary per-endpoint UDC TX
  diagnostics (`TX enqueue/complete ep 0xNN` lines; console ep 0x85 skipped).
- **CDC-ACM class logs can never be enabled while the console is CDC-ACM** —
  the recursion guard at `usbd_cdc_acm.c:26-35` forces `LOG_LEVEL_NONE` for
  the class. Put driver-side diagnostics in `udc_nrf.c` (INF) or the app
  layer instead.
- **Any UDC TX diagnostic must skip the console endpoint (0x85)** — logging
  the console's own TX creates a log feedback loop that floods and garbles
  the console (observed 2026-09-28).
- **Console output is garbled/delayed, but its timestamps are reliable**
  (captured at the call site in `z_log_msg_commit`); prefer short INF lines
  and don't trust line integrity during bursts.
- **The "flush on touch" clue, sharpened:** the UDC completes the response
  transfer at request time (~150 µs), yet Studio only acts on the bytes
  after a touchpad touch. If the raw capture confirms prompt arrival on the
  host, the prime suspect is the client's read loop stalling until UI
  activity (mouse movement re-driving the page), not the device.
- **Client under test:** `@zmkfirmware/zmk-studio-ts-client` v0.0.18
  (commit `fc53f31`), reference checkout at
  `/workspaces/ref/zmk-studio-ts-client`.
- **Client instrumentation (2026-09-28, uncommitted):** the reference
  client is instrumented for the timeout investigation — new `src/diag.ts`
  plus `diag()` calls in `framing.ts`, `index.ts`,
  `transport/serial.ts`. All lines are prefixed `[zmk-rpc]` with a
  relative timestamp (filter the browser console on `[zmk-rpc]`). Log
  layers, in pipeline order: `RAW TX` (browser → OS) → `RAW RX` (OS →
  browser) → `DECODER chunk` → `DECODER frame` / `DECODER ERROR` →
  `DECODED` (tee pulled the frame) → `RESPONSE/NOTIFICATION branch:
  consumed` (the app read it) → `call_rpc` write/read/`TIMEOUT`. A 10 s
  diagnostic read timeout was added to `call_rpc` (it releases the RPC
  mutex so testing can continue; remove for production). Build with
  `npm run build` (lib/ is up to date).
- **Pre-existing, unrelated boot errors — don't chase:** `adc_nrfx_saadc:
  Cannot configure channel 0: -22`, `vddh_init: VDDHDIV5 setup returned
  -22`, battery device "not ready", `settings: set-value failure` (present
  before the migration; verify against the pre-migration baseline if in
  doubt).

## FINAL ROOT CAUSE — 2026-09-30 (resolved, verified)

**The Studio-over-USB stall was an infinite self-resubmitting work loop in
the new upstream CDC-ACM TX implementation, not a remote-wakeup or RX
problem.**

### The bug

Upstream commit `6d69698de74` ("usb: device_next: cdc_acm: tx throughput
improvements", net_buf-based TX) left `cdc_acm_irq_cb_handler()`
re-submitting itself at the end of every run when:

```c
if (atomic_test_bit(&data->state, CDC_ACM_IRQ_TX_ENABLED) &&
    ring_buf_space_get(data->tx_fifo.rb)) {
    cdc_acm_work_submit(&data->irq_cb_work);
}
```

That condition is a **level, not an edge**: the app enables TX once
(`uart_irq_tx_enable`) and never disables it, and the TX ring is almost
always non-full when idle. So once the app sends its first response, the
handler re-invokes the app UART callback in a tight loop forever.

### Why it stalled everything

Both CDC-ACM instances (Studio RPC + console) share one work queue
(`cdc_acm_work_q`). The Zephyr work-queue thread (`work_queue_main` in
`kernel/work.c`) loops back to the pending list after each handler without
sleeping, so the self-resubmitting `irq_cb_work` hogs the queue and
starves/delays:

- `tx_fifo_work` — the work that actually drains the CDC-ACM TX ring to the
  wire (RPC responses stall), and
- the console instance's TX work (the log stream stalls in the same window).

That is why Studio *and* the console stalled together, and why a touchpad
touch (unrelated bus activity) appeared to "flush" things.

### Why it was so hard to find

- The CDC-ACM class module is force-compiled at `LOG_LEVEL_NONE` (the
  recursion guard at `usbd_cdc_acm.c:26-35`), so the loop ran **silently** —
  earlier captures showed "zero log lines" gaps that were actually the loop
  spinning, not an idle device.
- Adding `LOG_INF` to the app-side `serial_cb` finally made the loop
  visible: `serial_cb` invoked every ~3 ms with `rpc_tx_pending=0`
  (nothing to send), flooding the log until the backend dropped 9999
  messages.

### Fixes applied (zephyr fork, `usbd_cdc_acm.c`)

1. **Remove the unconditional TX re-submit** in `cdc_acm_irq_cb_handler()`
   (the loop source). TX-ready is still reported on the two real edges:
   `uart_irq_tx_enable()` and TX transfer completion
   (`usbd_cdc_acm_request()`).
2. **Empty-ring guard** in `cdc_acm_tx_fifo_handler()` — don't enqueue a
   spurious 0-byte transfer if the ring already drained.
3. `cdc_acm_fifo_fill()` schedules `tx_fifo_work` (the interrupt-driven
   write path never did; only `poll_out` did).
4. Suspend → `usbd_wakeup_request()` when TX is pending; resume → re-trigger
   pending TX and re-arm the bulk-OUT RX endpoint (resolves the upstream
   "resumed call (TODO)").
5. Clear `RX_FIFO_BUSY` on RX net_buf alloc/enqueue failure (a single
   failure otherwise wedges the RX endpoint permanently).
6. ZLP on max-packet boundary via `udc_ep_buf_set_zlp()` (replaces the
   old `zlp_needed` two-transfer scheme).

### Verification (2026-09-30)

- Normal Studio session: connects and stays connected; device log clean —
  169/169 `TX enqueue ep 0x83` matched by 169 `TX complete ep 0x83`
  (every response ACKed), 223/223 RX, **0 dropped log messages, 0 errors**,
  0 suspend/resume events.
- Stress test `stress_rpc_test.py`: **200/200 answered, 0 failed** in 2.9 s
  (~69 req/s), RX survived all 200 suspend/resume cycles.

### Cleanup status (closed 2026-09-30)

- Temporary diagnostics stripped: fork `udc_nrf.c` (per-endpoint
  TX/RX enqueue/complete lines) and app `uart_rpc_transport.c` (DIAG logs)
  are back to their pre-debug state. `usbd_cdc_acm.c` keeps only the real
  fixes (its `LOG_INF` lines are upstream's and compiled out by the
  `LOG_LEVEL_NONE` guard anyway).
- Fork fixes committed and pushed: `8789b5b8cb5` ("Fix USB + remove log")
  on `v4.4.1+zmk-fixes` (RyanDam/zephyr). The `app/west.yml` zephyr pin is
  that *branch*, so no pin bump was needed — pushing the branch is enough.
- Clean build re-verified after the strip: 570/570, 0 warnings,
  FLASH 322004 B / RAM 108452 B.
- **Optional follow-up (not blocking):** an idle test (leave Studio idle
  for minutes, then interact) to exercise the host-suspend → remote-wakeup
  path, which the captures above did not cover (no suspend events
  occurred). The suspend→`usbd_wakeup_request()` and resume→re-arm fixes
  are in place for it.
