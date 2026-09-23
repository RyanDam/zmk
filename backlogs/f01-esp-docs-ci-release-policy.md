# f01 — Phase 10: Documentation, CI policy, and support declaration

- **Category:** f (Cleanup & i18n)
- **Severity:** Medium
- **Status:** TODO
- **Effort (est):** M

## Problem
Once the ESP milestones land, the project has no published documentation, CI policy, or
release-gate evidence for the new targets: capability boundaries (C3 BLE-only, S3
conditional USB), flashing/recovery instructions, power methodology, settings-storage
behavior, and the Zephyr 4.4 upgrade's impact on out-of-tree integrations are all
undocumented.

## Evidence
- `implement_plan.md:393-421` — Phase 10 documentation, CI, and release-gate tasks.
- `report.md:231-237` — required CI and documentation additions (compile-only matrix entries, blob fetch policy, flashing/recovery via `west flash` including the reset/power-cycle caveat, qualified module flash sizes, hardware test results in the support declaration).
- `report.md:239-245` — go/no-go criteria per support level.

## Why it matters
- Users cannot safely adopt an experimental ESP port without explicit capability
  boundaries, recovery paths, and measured power data. Out-of-tree board/shield/module
  maintainers need the 4.4 migration impact documented (matrix input and USB especially).

## Conservative fix
- Add hardware integration pages for C3 and S3: exact target names, compatible modules, flash-size assumptions, pin reservations, matrix examples, flashing, serial monitor, reset/recovery, and blob fetch requirements.
- State capability boundaries clearly: C3 = BLE only, no USB HID; S3 = BLE, USB HID only on an explicit Zephyr-4.4-based, board-revision-specific, fuse-qualified variant if the USB-PHY gate passes; split/power = experimental until separately qualified.
- Publish the power methodology and results (d03), not just a headline current number.
- Document the settings storage backend, partition size, reset behavior, and migration/reset requirements.
- Document the Zephyr 4.4 upgrade impacts for out-of-tree boards/shields/modules, particularly the matrix input and USB migrations.
- CI: add compile targets for C3 BLE and S3 BLE; add the S3 USB HID target only after the USB-PHY gate passes, labeled fused/experimental in CI metadata; ensure west/module revision resolution is covered in clean CI environments; add automated tests for matrix input, BLE profile/settings logic, and the USB HID abstraction where feasible; maintain hardware-in-loop/manual release checklists for flash, BLE, USB, settings, and power tests.
- Adopt the release-gate table: Buildable / Experimental BLE / General BLE / S3 USB / Split, each with its required evidence.

## Out of scope (for now)
- Marketing or product-level claims; those follow the per-board support decisions from d03.

## Acceptance / verification
- [ ] C3 and S3 hardware integration pages published with exact targets, pin reservations, and flashing/recovery instructions.
- [ ] Capability boundaries documented (C3 BLE-only; S3 conditional USB; split/power experimental).
- [ ] Power methodology and results published.
- [ ] Settings storage backend, partition size, and reset/migration behavior documented.
- [ ] Out-of-tree 4.4 migration guide published (matrix input + USB).
- [ ] CI compile targets in place (C3 BLE, S3 BLE; S3 USB only if gated in, labeled fused/experimental).
- [ ] Release-gate evidence collected for each declared support level.
