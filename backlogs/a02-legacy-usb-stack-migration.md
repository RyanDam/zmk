# a02 — Phase 3: Migrate legacy USB device/HID stack

- **Category:** a (Architecture & maintainability)
- **Severity:** High
- **Status:** IN_PROGRESS (implementation + build validation done; hardware validation pending)
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
