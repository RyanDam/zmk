---
name: backlog-manager
description: >-
  Manage a file-based improvement backlog — one markdown file per item in a
  `backlogs/` directory, a `README.md` index, and a `done/` subfolder for finished
  items. Use this skill whenever the user wants to add a new backlog item,
  complete/close an item, mark one as won't-do, list or filter items by
  category/severity/status, keep the backlog index in sync, set up a backlog system
  in a project, or validate the backlog. Trigger on any mention of "backlog",
  "backlog item", "improvement backlog", "add an item to the backlog",
  "close/complete backlog item X", or the `backlogs/` directory — even if the user
  never says the word "skill".
---

# Backlog Manager

A backlog is a directory of self-contained markdown files — one per improvement
item — plus a `README.md` index and a `done/` subfolder. This skill keeps that
structure consistent across projects and does the error-prone mechanical work
(sequence numbers, file moves, status flips, index sync) via a bundled script so
the files and the index never drift apart.

## When to use
- Adding a new improvement item
- Completing an item (move to `done/`, flip status, sync the index)
- Marking an item won't-do
- Listing / filtering items by category, severity, or status
- Keeping the `README.md` index in sync (or repairing it)
- Bootstrapping a backlog system in a new project
- Validating the backlog (a quick health check, or wired into CI)

## Layout
```
backlogs/
  README.md                     index — one table per category
  <cat><NN>-<slug>.md           open items
  done/
    <cat><NN>-<slug>.md         completed items (same filename)
```

## Naming
`<category><NN>-<kebab-slug>.md`
- `<category>` — a single letter (table below).
- `<NN>` — two-digit, **per category** (resets per letter), zero-padded.
- `<kebab-slug>` — a short descriptive slug.

Example: `c01-hid-device-never-closed.md` = Correctness, item 01.

## Categories (fixed set)
| Prefix | Category |
|--------|----------|
| `c` | Correctness & reliability |
| `a` | Architecture & maintainability |
| `t` | Type safety |
| `d` | Testing & CI |
| `b` | Build, deploy & config |
| `f` | Cleanup & i18n |

Suggested work order: **c → d → a/t → b → f** (stop regressions shipping first,
then architecture/types, then build/deploy, then cleanup).

## Item template
Every item has a fixed front-matter block plus standard sections. See
`references/item-template.md` for the full blank template and a filled example.

Front-matter fields (each a `- **Key:** value` line near the top):
- **Category:** `<letter> (<name>)`
- **Severity:** High | Medium | Low
- **Status:** TODO | DONE | WON'T (intended)
- **Effort (est):** S | M | L

Sections, in order: `## Problem`, `## Evidence` (cite `path:line`),
`## Why it matters`, `## Conservative fix`, `## Out of scope (for now)` (optional),
`## Acceptance / verification` (checkboxes).

## Status rule
- **Open** items live in `backlogs/` with `Status: TODO`.
- **Done** items: `git mv` to `backlogs/done/` (keep the filename) and set
  `Status: DONE`.
- **Won't-do** items stay in `backlogs/`, with `Status: WON'T (intended)` and a
  `> **Decision:**` note explaining why the behavior is intended.
- The `README.md` index must always reflect the files: every add / complete /
  won't-do is a three-part change (file + status + index). The script bundles the
  status + index parts so you don't forget one.

## The helper script
`scripts/backlog.py` — stdlib-only Python 3.8+. It does the mechanical work so you
don't miscount a sequence number or desync the index. Run it from anywhere with
`--backlogs <dir>` (default `backlogs`).

| Command | What it does |
|---------|--------------|
| `list [--open] [--category X] [--status Y] [--severity Z]` | list items with parsed fields |
| `next <cat>` | print the next sequence number (scans open **and** done) |
| `new <cat> <slug> --title "..." [--severity S/M/L] [--effort S/M/L]` | create a new item from the template with the correct next number |
| `validate <file>` | validate one item (filename, front-matter, sections, status↔location) |
| `check` | validate every item + report duplicate sequence numbers |
| `done <file>` | set Status DONE, move to `done/` (git mv), rebuild the index |
| `wont <file> --note "..."` | set Status WON'T (intended), add the decision note, rebuild the index |
| `index` | rebuild the `README.md` index from the files on disk |

Prefer the script for these operations over hand-editing — it guarantees the
number, the move, the status, and the index stay in agreement.

## Workflows

### Set up a backlog in a new project
1. Create `backlogs/` and `backlogs/done/`.
2. Create `backlogs/README.md` from `references/readme-template.md` (header sections
   + an empty `## Index`).
3. Add the first item with `new`.
4. Run `index` to generate the first table.

### Add a new item
1. Pick a category letter, a kebab slug, and a concise title.
2. `python3 scripts/backlog.py new --backlogs <dir> <cat> <slug> --title "..." --severity <S/M/L> --effort <S/M/L>`
3. Fill in the sections — especially `## Evidence` with real `path:line` citations
   and `## Acceptance / verification` with checkable outcomes.
4. `python3 scripts/backlog.py index --backlogs <dir>` to add the row.
5. `python3 scripts/backlog.py validate --backlogs <dir> <file>` to confirm.

### Complete an item
1. `python3 scripts/backlog.py done --backlogs <dir> <file>` — flips Status to
   DONE, moves to `done/`, and rebuilds the index in one step.
2. Tick the `## Acceptance / verification` checkboxes in the item. The script
   doesn't do this for you — it's a judgment call about what was actually verified.

### Mark an item won't-do
`python3 scripts/backlog.py wont --backlogs <dir> <file> --note "why it's intended"`
— sets Status, adds the `> **Decision:**` note, and rebuilds the index.

### List / filter
`python3 scripts/backlog.py list --backlogs <dir> --open` (add `--category c`,
`--severity High`, `--status TODO`, etc.).

### Repair a drifted index
If the index ever drifts from the files, `python3 scripts/backlog.py index
--backlogs <dir>` regenerates it from disk. The files are the source of truth and
`index` is idempotent.

### Health check
`python3 scripts/backlog.py check --backlogs <dir>` — validates every item and
flags duplicate sequence numbers. Good to wire into CI.

## Gotchas
- **The index is generated from the files.** Never hand-edit a row without the
  file (or vice versa); when in doubt, run `index`.
- **Sequence numbers reset per category**, and `next`/`new` scan *both*
  `backlogs/` and `done/` so a number can't collide with a finished item.
- **`done` uses `git mv` when the file is tracked** (preserves rename detection)
  and falls back to a plain move otherwise. Commit the result.
- **Status must match location.** `check`/`validate` flag a file in `done/` that
  isn't `DONE`, or a `DONE` file that isn't in `done/`.
- **Won't-do items are not moved** — they stay in `backlogs/` with the decision
  note, so the reasoning lives next to the item.
