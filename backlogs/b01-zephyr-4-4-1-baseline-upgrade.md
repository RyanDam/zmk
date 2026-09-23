# b01 — Phase 1: Upgrade Zephyr baseline to 4.4.1

- **Category:** b (Build, deploy & config)
- **Severity:** High
- **Status:** TODO
- **Effort (est):** L

## Problem
ZMK is pinned to Zephyr `v4.1.0+zmk-fixes`. Zephyr 4.1 lacks the ESP32-S3 `usb_otg`/DWC2
controller node (blocking S3 wired HID), and the kscan subsystem was removed in 4.2. The
recommended ESP baseline is Zephyr 4.4.1 (S3 USB-OTG support, Espressif Bluetooth/PSRAM
fixes, supported upstream until 2027-04-12), but the repository-wide upgrade has not been
done.

## Evidence
- `app/west.yml` — pins Zephyr to `v4.1.0+zmk-fixes`.
- `report.md:112` — the 4.1 S3 SoC DTS has only USB Serial/JTAG; no `usb_otg`/DWC2 node.
- `report.md:31` — Zephyr removed the whole kscan subsystem in 4.2 (commit `60a9a202`), so a 4.4 build fails immediately on the kscan API.
- `implement_plan.md:79-117` — Phase 1 tasks, files, and exit gate.

## Why it matters
- It is a prerequisite for every later ESP milestone: the S3 USB path needs the 4.4 DWC2
  UDC model, and the kscan/USB migrations (a01, a02) target the 4.4 API. Doing it as a
  standalone, reviewable change keeps ESP regressions attributable.

## Conservative fix
- Pin Zephyr to exactly `v4.4.1` (never an unbounded `main`).
- Rebase the ZMK Zephyr fork/patches onto `v4.4.1`; classify every patch as upstreamed / still-needed / needs-port / obsolete.
- Update `app/west.yml` and import the matching Zephyr 4.4 west dependencies — in particular the matching `hal_espressif` revision (do not mix a 4.1 HAL with 4.4 Zephyr).
- Update only ZMK-pinned module revisions that must match the new Zephyr API (e.g. LVGL); do not bump unrelated modules.
- Update the development environment and CI: `.devcontainer/`, GitHub workflow Python installs and cache keys, Zephyr SDK >= 1.0.0, Python >= 3.12, C17 flags, west and pip dependencies.
- Run `west update`, `west blobs fetch hal_espressif`, and `west packages pip --install` in a clean workspace; document both source-only (`BUILD_ONLY_NO_BLOBS`) and blob-backed workflows.
- Apply the Zephyr 4.2, 4.3, and 4.4 migration guides cumulatively, tracking each change against this repository.

## Out of scope (for now)
- kscan → matrix input migration (a01), USB stack migration (a02), and any ESP board code (b02/b03) — deliberately separate, ordered milestones.

## Acceptance / verification
- [ ] The resolved west manifest reproducibly contains the expected Zephyr `v4.4.1` and matching `hal_espressif` SHAs.
- [ ] Representative builds pass: a USB+BLE Nordic board, an RP2040 board, an STM32 board, a native test board, and a split board.
- [ ] Every downstream Zephyr fix is ported, upstreamed, or consciously dropped with a recorded rationale.
- [ ] Devcontainer and CI run on SDK 1.0 / Python 3.12 / C17 with no stale toolchain dependency hidden by a warm cache.
