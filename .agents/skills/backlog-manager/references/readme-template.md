# README template

Use this as the starting `backlogs/README.md` when bootstrapping a backlog in a new
project. Replace `<Project name>`. Keep the section headings — the `## Index`
section is what `backlog.py index` regenerates, so leave it (or just the heading)
in place and let the script fill the tables.

```markdown
# Backlogs

Improvement backlog for <Project name>. Each file is one self-contained item.

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

## Index

<!-- `backlog.py index` regenerates everything below this line. -->
```

After creating this file, add your first item with `backlog.py new ...` and run
`backlog.py index` — the tables appear under `## Index` automatically, one per
category, sorted by sequence number.
