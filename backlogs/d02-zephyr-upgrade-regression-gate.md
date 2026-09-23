# d02 — Phase 4: Whole-project Zephyr upgrade regression gate

- **Category:** d (Testing & CI)
- **Severity:** High
- **Status:** TODO
- **Effort (est):** M

## Problem
The Zephyr 4.4.1 upgrade plus the kscan and USB migrations have a broad regression blast
radius across every ZMK board, shield, display, sensor, split transport, bootloader, and
Studio integration. There is no gate proving the upgraded baseline is healthy before ESP
support is layered on top.

## Evidence
- `implement_plan.md:207-227` — Phase 4 tasks and exit gate.
- `report.md:156` — "Broader regression blast radius … Use staged PRs and a full board/shield build matrix. The ESP port must not become the justification for accepting unrelated regressions."
- `implement_plan.md:50` — Phases 1–4 are deliberately ahead of ESP board code so regressions stay attributable.

## Why it matters
- The upgrade must stand on its own as a release candidate. Merging ESP work on top of an
  unverified upgrade would make any failure (ESP vs upgrade) impossible to attribute.

## Conservative fix
- Run the complete automated suite, all configured CI build targets, devicetree/hardware metadata validation, formatting, and static checks.
- Produce a board-by-board migration failure list; fix or explicitly quarantine only boards already unsupported upstream — do not silently reduce claimed support.
- Run physical smoke tests for representative combinations: BLE-only, USB+BLE, wired split, wireless split, display, RGB/LED, encoder/pointing input, and settings reset/retained boot behavior.
- Validate persistent settings format behavior across firmware upgrade/downgrade for representative NVS/FCB users; document any required settings reset.
- Review resulting binary RAM/flash changes and stack high-water marks for constrained existing boards.

## Out of scope (for now)
- ESP board work (b02/b03) — starts only after this gate passes.

## Acceptance / verification
- [ ] Full CI (all build targets, tests, DT/metadata validation, formatting, static checks) is green on 4.4.1.
- [ ] Board-by-board migration failure list exists and is resolved or explicitly quarantined with rationale.
- [ ] Physical smoke tests pass for all representative combinations listed above.
- [ ] Settings persistence across upgrade/downgrade validated and any required reset documented.
- [ ] RAM/flash/stack deltas reviewed for constrained boards.
- [ ] The Zephyr upgrade can stand on its own as a release candidate.
