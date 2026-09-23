# a01 — Phase 2: Migrate legacy kscan to matrix input

- **Category:** a (Architecture & maintainability)
- **Severity:** High
- **Status:** TODO
- **Effort (est):** L

## Problem
ZMK's keyboard scanning is built on the Zephyr kscan subsystem, which was removed
entirely in Zephyr 4.2. This tree still has seven legacy kscan translation units and
kscan consumers in physical-layout/sideband code, so any 4.4 build fails immediately on
`#include <zephyr/drivers/kscan.h>`, `CONFIG_KSCAN`, and `struct kscan_driver_api`.

## Evidence
- `app/module/drivers/kscan/` — seven legacy kscan drivers (matrix, direct, demux, charlieplex, mock, composite, gpio support) exposing `kscan_driver_api`.
- `app/src/physical_layouts.c` — still includes `kscan.h` and carries the legacy callback/lifecycle path, though it already accepts `zmk,matrix-input` and installs Input callbacks.
- `app/src/kscan_sideband_behaviors.c`, `app/include/zmk/physical_layouts.h` (`struct zmk_physical_layout` retains `kscan`), `app/include/zmk/matrix.h` — legacy consumers.
- `report.md:31-44` — kscan removal (commit `60a9a202`) and the kscan vs Input event-model differences.

## Why it matters
- Hard build blocker on Zephyr 4.2+/4.4 and a prerequisite for the ESP port. Carrying a
  private compatibility copy of a removed subsystem would create a permanent, unmaintained
  fork. This is an API and behavior migration, not a mechanical include rename.

## Conservative fix
- Inventory all use of `kscan.h`, `CONFIG_KSCAN`, kscan devicetree bindings, callbacks, and test fixtures, including all board/shield overlays.
- Make the existing Input path in `app/src/physical_layouts.c` the sole path: producers emit `INPUT_ABS_X` (column), `INPUT_ABS_Y` (row), then a synchronized `INPUT_BTN_TOUCH`; the listener maps the triplet through the active layout's matrix transform. Replace callback enable/disable with tested device-power/layout filtering.
- Port one driver at a time: direct GPIO matrix fixture first, then matrix/direct/charlieplex/demux/composite, then mocks/test-only drivers. Upstream `gpio-kbd-matrix` may replace the ordinary matrix case after equivalence testing.
- Preserve keyboard behavior: debounce, diode direction, wake-up interrupt behavior, scan timing/idle state, ghosting semantics, sideband behaviors, and split-half roles.
- Convert or replace associated devicetree bindings and overlays; remove the legacy Kconfig selection only after all consumers are migrated.
- Expand unit/native tests to assert (not just compile-test) the three-event ordering/sync boundary, press/release order, debounce, simultaneous presses, and sleep/wake events.

## Out of scope (for now)
- USB stack migration (a02) and ESP board enablement (b02/b03).

## Acceptance / verification
- [ ] No production ZMK driver depends on the removed kscan API.
- [ ] Native/unit tests pass for each driver family, including the three-event ordering and synchronization boundary.
- [ ] Physical smoke tests pass on at least one board per driver family.
- [ ] Every in-tree board/shield that uses an affected driver still builds.
- [ ] A held key through USB and BLE, wake from idle, and matrix rollover behavior verified on hardware.
