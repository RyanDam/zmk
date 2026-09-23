# a04 — Phase 9b: ESP battery, display, RGB, sensors, pointing (experimental)

- **Category:** a (Architecture & maintainability)
- **Severity:** Low
- **Status:** TODO
- **Effort (est):** M

## Problem
Beyond the base BLE keyboard, the ESP targets have no validated support for additional
peripheral classes: battery monitoring, displays, RGB, sensors, or pointing. Each class
can consume meaningful RAM/current on 400/512 KB SRAM devices and can interfere with
BLE/HID operation.

## Evidence
- `implement_plan.md:383-385` — Phase 9 "Battery, display, RGB, sensors, and pointing": enable one peripheral class at a time; each must demonstrate no unacceptable RAM/current impact and no interference with BLE/HID; use board-specific overlays rather than expanding the generic development-board default.

## Why it matters
- These features are the difference between a development board definition and a usable
  keyboard product, but each is independently risky on constrained ESP hardware and must
  not be bundled into one unreviewable change.

## Conservative fix
- Enable one peripheral class at a time (battery, display, RGB, sensors, pointing).
- For each class: demonstrate no unacceptable RAM/current impact and no interference with BLE/HID operation.
- Add board-specific overlays rather than expanding the generic development-board default.

## Out of scope (for now)
- Combining multiple peripheral classes in one change set; production battery claims (still gated on d03 power data).

## Acceptance / verification
- [ ] Each enabled peripheral class has measured RAM and current impact recorded.
- [ ] No BLE/HID interference demonstrated for each enabled class.
- [ ] Changes land as board-specific overlays, one peripheral class per change set.
