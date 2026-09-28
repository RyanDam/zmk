# a02 — Phase 3: Migrate legacy USB device/HID stack

- **Category:** a (Architecture & maintainability)
- **Severity:** High
- **Status:** TODO
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
- [ ] Keyboard boot/report protocol switching, consumer and mouse reports, suspend/resume, unplug/replug, and reset-while-connected all pass.
- [ ] Existing USB logging and Studio configurations work where supported.
      (Build-verified: both snippets compile and register their CDC ACM
      instances on the new stack; hardware check pending.)
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

Client-side findings (`zmk-studio-ts-client`):
- `src/transport/serial.ts:7` opens the port at `baudRate: 12500` (virtual
  for CDC-ACM; should be inert).
- `src/framing.ts` decoder is all-or-nothing: any byte ≠ `0xAB` in the IDLE
  state, or a `0xAB` mid-frame, calls `controller.error()` and kills the
  whole readable stream until reconnect — one stray or partial frame kills
  the session.
- `src/index.ts:103` `call_rpc` has no read timeout; `tee()` backpressure
  can stall the source if the notification branch is not consumed.

Ruled out: endpoint shortage, net-buf pool exhaustion, UDC XFER-event loss,
stuck `TX_FIFO_BUSY` (self-heals on completion), host-side ZLP holding,
device suspend (no SUSPEND events), CDC-ACM class TX stall (UDC completes
promptly).

### Potential ways forward
1. **Raw byte capture on the host (decisive, ~2 min).** With Studio
   disconnected: `timeout 20 cat /dev/ttyACM<n> > /tmp/rpc_raw.bin` (the RPC
   port, not "ZMK Logging"), generate traffic (keys/touchpad), then `xxd`.
   Clean `ab … ad` frames → host/Studio-side fault (check Studio's browser
   console for decoder errors, verify the attached port, `tee()`
   backpressure). Garbled bytes or a missing `ad` → device framing
   corruption; fix the `rpc.c` ring/escape path (incl. the
   `claim_len == 1` escape edge case at `rpc.c:162-169`).
2. **Host-side investigation.** Check Studio's browser console for
   "Expected SoF to start decoding" / "Unexpected SoF mid-frame"; verify
   Studio is attached to "ZMK Studio RPC", not "ZMK Logging".
3. **A/B against the legacy stack (low prior).** Both stacks exist in this
   Zephyr; a `CONFIG_USB_DEVICE_STACK` build would confirm whether the
   symptom is stack-specific. Low prior because framing is produced by the
   app, not the stack.
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
- **Pre-existing, unrelated boot errors — don't chase:** `adc_nrfx_saadc:
  Cannot configure channel 0: -22`, `vddh_init: VDDHDIV5 setup returned
  -22`, battery device "not ready", `settings: set-value failure` (present
  before the migration; verify against the pre-migration baseline if in
  doubt).
