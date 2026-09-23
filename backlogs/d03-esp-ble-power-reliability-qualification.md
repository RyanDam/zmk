# d03 — Phase 7: ESP BLE reliability and power qualification

- **Category:** d (Testing & CI)
- **Severity:** High
- **Status:** TODO
- **Effort (est):** L

## Problem
Battery life is the primary product risk for the ESP targets: Espressif's status matrix
lists C3/S3 low power as work in progress, and an earlier ZMK S3 proof of concept reported
~50–100 mA before being abandoned. Neither chip has measured power data or demonstrated
long-duration BLE reliability on ZMK.

## Evidence
- `report.md:17` — earlier S3 PoC reported roughly 50–100 mA and was abandoned because of power and USB gaps.
- `report.md:202` — the Espressif BT driver reserves a 25,600-byte heap addition by default plus a 4,096-byte controller task stack, which is significant on 400/512 KB SRAM devices.
- `implement_plan.md:313-338` — Phase 7 tasks, acceptance criteria, and exit gate.

## Why it matters
- The measured data decides each board's support level: experimental, externally powered
  only, or suitable for the stated battery use case. No battery claims may be made before
  the full idle/advertising/connected/sleep lifecycle is measured.

## Conservative fix
- Instrument C3 and S3 builds to capture Bluetooth/controller initialization, heap allocation failures, settings write failures, HCI errors, disconnect reasons, and reset causes.
- Measure memory after boot and after `bt_enable()`, during pairing, active typing, and after settings writes; record stack high-water marks.
- Tune only measurements-proven values: Espressif controller task stack, Bluetooth host buffers, ZMK BLE report queues, connection intervals, preferred peripheral latency.
- Measure current at 3.3 V for: boot, open advertising, connected idle, key burst, reconnect, light/deep sleep attempts, settings writes, and powered-off/soft-off state.
- Run at least 24-hour idle/reconnect tests and a multi-day typing/reconnect soak test for each chip.
- Test difficult host behavior: host sleep/wake, Bluetooth toggled off/on, profile handoff, bond overwrite, host out of range, and radio congestion.
- Set numeric current and reliability targets before testing begins; do not label a battery configuration supported until measured current meets a published use-case budget.

## Out of scope (for now)
- Product marketing claims; split qualification (a03) and USB (b04) remain separate gates.

## Acceptance / verification
- [ ] Numeric current and reliability targets defined and recorded before testing starts.
- [ ] No unhandled resets and no persistent pair/bond corruption across the test campaigns.
- [ ] Recovery without reflashing after host disconnects, with reproducible logs for all failures.
- [ ] Measured power/reliability data published: test circuit, firmware SHA, radio settings, sample size, median/peak current.
- [ ] A separate, documented support decision made for each board (experimental / externally powered / battery-suitable).
