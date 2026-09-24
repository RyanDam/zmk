# Backlogs

Improvement backlog for ZMK ESP32-C3 / ESP32-S3 support (Zephyr 4.4.1 upgrade + ESP port).
Each file is one self-contained milestone item. Source documents: [`../report.md`](../report.md)
and [`../implement_plan.md`](../implement_plan.md).

## Naming convention

`<category><NN>-<kebab-slug>.md`

- `<category>` is a single letter (see table below).
- `<NN>` is a two-digit number, **per category** (resets per letter), zero-padded.
- `<kebab-slug>` is a short descriptive slug.

Example: `c01-fix-race-condition.md` = Correctness, item 01.

## Category prefixes

| Prefix | Category |
|--------|----------|
| `c` | Correctness & reliability |
| `a` | Architecture & maintainability |
| `t` | Type safety |
| `d` | Testing & CI |
| `b` | Build, deploy & config |
| `f` | Cleanup & i18n |

## Status rule

- **Open** items live in `backlogs/` with `Status: TODO`.
- **Done** items are moved to `backlogs/done/` (keep the filename) with
  `Status: DONE`.
- **Won't implement** items stay in `backlogs/` with `Status: WON'T (intended)`
  and a `> **Decision:**` note.

## How to parse (for tooling / agents)

- Filename → category (first char) + sequence (next two digits) + slug (rest).
- Each file has a fixed front-matter block of `- **Key:** value` lines:
  `Category`, `Severity` (High/Medium/Low), `Status` (TODO/DONE/WON'T (intended)),
  `Effort (est)` (S/M/L).
- Open items: `ls backlogs/*.md` (everything not in `backlogs/done/`).
- By category: `ls backlogs/<prefix>*.md` (e.g. `backlogs/c*.md`).
- High-severity open items: grep `- **Severity:** High` in `backlogs/*.md`.

## Suggested order

Work the categories in this order: **c → d → a/t → b → f**.

Note: for this backlog the items are also an ordered delivery sequence — follow the
phase numbers in `implement_plan.md` (Phase 0 → Phase 10) rather than the generic
category order.

## Index

### c — Correctness & reliability
| File | Title | Severity | Effort | Status |
|------|-------|----------|--------|--------|
| [c01](done/c01-indicator-led-dt-requirement-breaks-builds.md) | indicator.c requires LED devicetree unconditionally, breaking all test and upstream board builds | High | S | DONE |

### a — Architecture & maintainability
| File | Title | Severity | Effort | Status |
|------|-------|----------|--------|--------|
| [a01](a01-kscan-to-matrix-input-migration.md) | Phase 2: Migrate legacy kscan to matrix input | High | L | TODO |
| [a02](a02-legacy-usb-stack-migration.md) | Phase 3: Migrate legacy USB device/HID stack | High | L | TODO |
| [a03](a03-esp-wireless-split-ble.md) | Phase 9a: ESP wireless split BLE (experimental) | Low | L | TODO |
| [a04](a04-esp-battery-display-rgb-peripherals.md) | Phase 9b: ESP battery, display, RGB, sensors, pointing (experimental) | Low | M | TODO |
| [a05](a05-esp-mcuboot-secure-boot-ota.md) | Phase 9c: ESP MCUboot / secure boot / OTA | Low | L | TODO |

### d — Testing & CI
| File | Title | Severity | Effort | Status |
|------|-------|----------|--------|--------|
| [d01](d01-esp-port-baseline-hardware-fixtures.md) | Phase 0: Establish reproducible baseline and hardware fixtures | Medium | M | TODO |
| [d02](d02-zephyr-upgrade-regression-gate.md) | Phase 4: Whole-project Zephyr upgrade regression gate | High | M | TODO |
| [d03](d03-esp-ble-power-reliability-qualification.md) | Phase 7: ESP BLE reliability and power qualification | High | L | TODO |

### b — Build, deploy & config
| File | Title | Severity | Effort | Status |
|------|-------|----------|--------|--------|
| [b01](b01-zephyr-4-4-1-baseline-upgrade.md) | Phase 1: Upgrade Zephyr baseline to 4.4.1 | High | L | TODO |
| [b02](b02-esp32c3-supermini-ble-board.md) | Phase 5: Add ESP32-C3 SuperMini BLE board variant | High | M | TODO |
| [b03](b03-esp32s3-supermini-ble-board.md) | Phase 6: Add ESP32-S3 SuperMini BLE board variant | High | L | TODO |
| [b04](b04-esp32s3-usb-hid-conditional.md) | Phase 8: ESP32-S3 SuperMini USB HID (conditional, USB-PHY gate) | Medium | L | TODO |

### f — Cleanup & i18n
| File | Title | Severity | Effort | Status |
|------|-------|----------|--------|--------|
| [f01](f01-esp-docs-ci-release-policy.md) | Phase 10: Documentation, CI policy, and support declaration | Medium | M | TODO |
