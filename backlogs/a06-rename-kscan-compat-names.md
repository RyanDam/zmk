# a06 — Rename zmk,kscan-* compat names to zmk,input-*

- **Category:** a (Architecture & maintainability)
- **Severity:** Low
- **Status:** TODO
- **Effort (est):** M

## Problem
a01 migrated the kscan subsystem to the Zephyr Input API but deliberately kept the
historical DT surface (`zmk,kscan-*` compatibles, the `zmk,kscan` chosen, and the
`kscan` property on `zmk,physical-layout`) so existing board/shield devicetrees keep
building unchanged. The names now misdescribe the hardware: these are Input API
producers, not kscan devices. The legacy names also keep the word "kscan" in Kconfig
symbols (`ZMK_KSCAN_*`), the driver directory, and the msgq name in
`physical_layouts.c`.

## Evidence
- `app/module/dts/bindings/kscan/zmk,kscan-gpio-matrix.yaml:7` — driver binding still
  named `zmk,kscan-gpio-matrix` although the driver emits `INPUT_EV_ABS`/`INPUT_EV_KEY`
  events.
- `app/dts/bindings/zmk,physical-layout.yaml:22` — the `kscan` property is documented
  as legacy fallback next to the `input` property.
- `app/src/physical_layouts.c` — `LAYOUT_INPUT_NODE()` resolves `input` → `kscan` →
  `zmk,matrix-input` chosen → `zmk,kscan` chosen; the last two hops exist only for
  legacy DTs.
- `app/module/drivers/kscan/Kconfig` — `ZMK_KSCAN_*` symbols for Input-API drivers.
- 47 in-tree devicetree files still use a `zmk,kscan*` compatible, plus the external
  zmk-config repo (cobanpad16a/cobanpad12b overlays).

## Why it matters
- New contributors see "kscan" and reach for the removed Zephyr kscan API; the name
  is a migration artifact that perpetuates confusion.
- The dual-name resolution chain in `physical_layouts.c` / `matrix.h` /
  `matrix_transform.c` is dead weight once all DTs use the new names.

## Conservative fix
1. Add the new `zmk,input-*` compatibles in parallel (bindings + driver
   instantiation for both names; keep `zmk,kscan` chosen and `kscan` property as
   deprecated aliases of `zmk,matrix-input` / `input`).
2. Migrate all in-tree devicetrees to the new names (bindings, chosen, property).
3. Coordinate the zmk-config migration (cobanpad16a/cobanpad12b overlays) in the same
   or a follow-up change.
4. Remove the legacy compatibles/bindings/aliases and the legacy resolution hops in
   `physical_layouts.c`, `matrix.h`, `matrix_transform.c`; rename
   `app/module/drivers/kscan/` → `.../input/` and the `ZMK_KSCAN_*` Kconfig symbols.

## Out of scope (for now)
- Renaming `CONFIG_ZMK_KSCAN_EVENT_QUEUE_SIZE` consumers beyond the Kconfig symbol
  itself (the msgq is internal to `physical_layouts.c`).
- Any behavioral change to event encoding (X=column, Y=row, BTN_TOUCH, sync).

## Acceptance / verification
- [ ] No `zmk,kscan*` compatible, `zmk,kscan` chosen, or `kscan` property remains in
      `app/` devicetrees or bindings.
- [ ] zmk-config cobanpad16a/cobanpad12b build with the new names
      (`bash app/build_coban.sh`).
- [ ] `./run-test.sh all` passes (test boards migrated to the new names).
- [ ] `app/module/drivers/kscan/` renamed; no `ZMK_KSCAN_*` Kconfig symbols remain.
