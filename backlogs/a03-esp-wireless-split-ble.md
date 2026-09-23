# a03 — Phase 9a: ESP wireless split BLE (experimental)

- **Category:** a (Architecture & maintainability)
- **Severity:** Low
- **Status:** TODO
- **Effort (est):** L

## Problem
ZMK's split BLE code is SoC-neutral and a historical S3 proof of concept reported split
operation, but split has never been validated on the ESP port. It stresses central and
peripheral roles, scans, reconnects, settings persistence, and radio coexistence — none of
which are covered by single-board BLE qualification.

## Evidence
- `implement_plan.md:376-381` — Phase 9 "Wireless split BLE" tasks.
- `report.md:208` — validate split after single-board BLE; start with S3 (more RAM), two-half fixed fixture, no display/RGB, explicit throughput/latency tests.

## Why it matters
- Split is the most complex radio configuration on a constrained BLE stack; shipping it
  unqualified would risk disconnect-recovery failures on user hardware. It must remain
  experimental until separately qualified.

## Conservative fix
- Start with S3 due to its greater RAM; use a fixed two-half fixture.
- Validate central/peripheral reconnect, settings persistence, boot ordering, profile selection, and throughput under typing.
- Run soak tests with one/both halves reset or out of range.
- Qualify C3 only after S3 passes; do not assume parity.
- Do not advertise split as production-ready until disconnect recovery survives extended tests.

## Out of scope (for now)
- C3 split qualification (deferred until S3 passes), wired split, and any split+display/RGB combinations.

## Acceptance / verification
- [ ] Split operates on S3 with the fixed two-half fixture (reconnect, persistence, boot ordering, profiles, typing throughput).
- [ ] Soak tests with one/both halves reset or out of range pass.
- [ ] Disconnect recovery survives extended testing before any production-ready claim.
- [ ] C3 split qualified separately or explicitly deferred.
