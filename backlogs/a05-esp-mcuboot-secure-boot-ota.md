# a05 — Phase 9c: ESP MCUboot / secure boot / OTA

- **Category:** a (Architecture & maintainability)
- **Severity:** Low
- **Status:** TODO
- **Effort (est):** L

## Problem
There is no defined boot, secure-boot, or OTA story for the ESP targets: MCUboot image
generation, partition layout, recovery, signing, and upgrade rollback have not been
verified against Espressif's sysbuild flow.

## Evidence
- `implement_plan.md:387-389` — Phase 9 "MCUboot / secure boot / OTA": treat as a dedicated boot/partition project; verify Espressif sysbuild/MCUboot images, partition layout, recovery, signing, and upgrade rollback before documenting support.

## Why it matters
- OTA and secure boot are a separate, dedicated project with its own partition and
  recovery risks. Documenting support before verification would risk users being unable to
  recover a bricked board.

## Conservative fix
- Treat as a dedicated boot/partition project, separate from the BLE/USB milestones.
- Verify Espressif sysbuild/MCUboot images and the resulting partition layout.
- Verify recovery, signing, and upgrade rollback before documenting support.

## Out of scope (for now)
- Secure-boot enrollment workflows and production OTA infrastructure — verify the firmware-side flow first.

## Acceptance / verification
- [ ] MCUboot image builds and boots on the target via the Espressif sysbuild flow.
- [ ] Partition layout documented and consistent with the settings partition used by b02/b03.
- [ ] Recovery, signing, and upgrade rollback all tested and passing before support is documented.
