# Item template

Every backlog item is a single self-contained markdown file. The H1 carries the
item id and title; the front-matter block is a fixed set of `- **Key:** value`
lines; then the standard sections follow in this order.

## Blank template

```markdown
# <cat><NN> — <Concise item title>

- **Category:** <cat> (<Category name>)
- **Severity:** <High | Medium | Low>
- **Status:** TODO
- **Effort (est):** <S | M | L>

## Problem
What is wrong, in plain terms. Describe the behavior, not the fix.

## Evidence
- `path/to/file.ts:LINE` — what the code does and why it is a problem.
- `path/to/other.ts:LINE` — a second data point, if any.

## Why it matters
- The user-visible or maintainability impact. Who is hurt, and how?

## Conservative fix
- The smallest change that resolves the problem. Prefer a few concrete bullets.

## Out of scope (for now)
- What is deliberately left out of this item (optional section).

## Acceptance / verification
- [ ] A concrete, checkable outcome (a command that passes, a behavior you can observe).
- [ ] Another checkable outcome.
```

Notes:
- `<cat><NN>` in the H1 must match the filename (e.g. `c09` ↔ `c09-....md`).
- The H1 uses an em dash: `# c09 — Title` (the script parses the title from here).
- `## Evidence` should cite real `path:line` locations — this is what makes an
  item actionable and verifiable later.
- `## Acceptance / verification` uses markdown checkboxes; tick them as you
  complete the item.

## Filled example

```markdown
# c05 — GIF upload: "save EEPROM" block re-sends the GIF-save command

- **Category:** c (Correctness & reliability)
- **Severity:** High
- **Status:** DONE
- **Effort (est):** S

## Problem
Two adjacent blocks in the GIF upload flow are labeled "Save GIF data to flash"
and "Save EEPROM data to flash", but both send the identical command
`[0x09, 0x00, 0x97]`. The second block should send the EEPROM-save command
(`0x96`) but instead re-sends the GIF-save command — a copy-paste divergence.

## Evidence
- `frontend/src/app/protocols/usb-hid-device-protocol.ts:343-352` — "Save GIF data":
  sends `[0x09, 0x00, 0x97]`.
- `frontend/src/app/protocols/usb-hid-device-protocol.ts:354-363` — "Save EEPROM data":
  sends the **same** `[0x09, 0x00, 0x97]` (should be `0x96`).

## Why it matters
Silent data corruption: the user believes EEPROM settings were saved, but the wrong
command is issued, so the change is lost on reboot.

## Conservative fix
- Change the second block to send `0x96`.
- Verify the intended command ID against the firmware / command constants first.

## Out of scope (for now)
- Refactoring the whole GIF upload flow into named steps.

## Acceptance / verification
- [x] The EEPROM-save command byte is `0x96` in the "Save EEPROM data" block.
- [ ] Reboot the device and confirm the EEPROM change persisted. _(manual — hardware)_
- [x] `tsc --noEmit` introduces no new type errors.
```

## Won't-do example (front-matter + decision note only)

```markdown
# a06 — Hardcoded device data lives in Next API routes

- **Category:** a (Architecture & maintainability)
- **Severity:** Low
- **Status:** WON'T (intended)
- **Effort (est):** M

> **Decision:** Won't implement — serving per-device data from the API routes is intended.

## Problem
...
```

The `> **Decision:**` line sits between the front-matter block and `## Problem` and
records *why* the item is being closed without implementation.
