# d01 — Phase 0: Establish reproducible baseline and hardware fixtures

- **Category:** d (Testing & CI)
- **Severity:** Medium
- **Status:** DONE
- **Effort (est):** M

## Problem
There is no reproducible hardware or software baseline for the ESP32-C3/S3 port: no
qualified reference boards, no matrix keyboard fixture, no power-measurement setup, no
recorded pre-upgrade CI/build state, and no agreed test-results format. "SuperMini" is a
third-party form factor with vendor variation, so "a SuperMini" is not a sufficient
hardware identifier.

## Evidence
- `implement_plan.md:54-75` — Phase 0 tasks and exit gate.
- `report.md:65` — flash/PSRAM, LED, battery, and USB wiring vary by seller; support must be defined against an explicitly qualified reference sample.
- `report.md:17` — an earlier ZMK S3 proof of concept reported ~50–100 mA and was abandoned; measurements are the primary product risk.
- `app/west.yml` — current `v4.1.0+zmk-fixes` pin that must be recorded as the baseline before changing.

## Why it matters
- Without qualified hardware and an archived baseline, regressions during the Zephyr 4.4
  upgrade and ESP bring-up cannot be attributed, and go/no-go criteria cannot be evaluated.

## Conservative fix
- Create an upgrade/port tracking branch; record the current ZMK commit, the `v4.1.0+zmk-fixes` base, the active ZMK Zephyr patch series, and all west module revisions.
- Acquire two known-good dev boards, USB cables, a serial monitor path, a 3.3 V supply, and a current meter/power profiler.
- Build a simple direct-GPIO matrix fixture that avoids flash, boot strapping, UART/JTAG, onboard LED, and native USB pins.
- Photograph and record silkscreen, chip marking, PCB revision, flash/PSRAM size, BOOT/RESET/LED wiring, and USB-C D+/D- route for every reference board.
- Confirm C3 USB-C is USB Serial/JTAG on GPIO18/19 and S3 USB-C is routed to GPIO19/20.
- Prepare host test machines or repeatable procedures for Linux, macOS, Windows, Android, and iOS where available.
- Record baseline CI results, release build sizes, and test coverage before changing the Zephyr pin.
- Define the test-results format: board/module revision, chip marking, flash/PSRAM, USB-PHY/eFuse state, firmware SHA, Zephyr/hal_espressif SHA, toolchain versions, host OS, test case, duration, result, logs, measured current.

## Out of scope (for now)
- Any firmware, manifest, or Zephyr pin changes — this milestone is fixtures and records only.

> **Closed 2026-09-26 (owner decision):** Phase 0 closed with the exit gate
> met (baseline archived, flash/monitor proven on unit 01 of both chips) and
> the following items intentionally skipped — they remain open work for the
> phases that need them:
>
> - **Matrix fixture:** pin map documented in
>   `esp-port/fixtures/matrix-fixture.md`, but the physical fixture was not
>   built. Needed before Phase 5/6 board bring-up testing.
> - **Reference board intake:** photos, full chip-marking transcription,
>   PCB revision, wiring continuity evidence, S3 eFuse `USB_PHY_SEL` dump,
>   and unit 02 flash/monitor are still pending (unit 01 of each chip is
>   recorded in `esp-port/test-results/`).
> - **Power-measurement setup:** 3.3 V supply + current meter/power profiler
>   not acquired. Needed before Phase 7 power qualification.

## Acceptance / verification
- [x] Tracking branch exists with the recorded baseline (ZMK commit, west module revisions, patch list).
- [x] At least one developer can flash/monitor each ESP development board independently.
- [ ] Matrix fixture built and documented with its pin map and conflict notes.
- [ ] Reference boards fully documented (chip marking, flash/PSRAM, USB route, PCB revision).
- [x] Baseline CI results and build sizes archived.
- [x] Test-results format defined and demonstrated with at least one sample entry.
