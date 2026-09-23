#!/usr/bin/env python3
"""backlog.py — manage a file-based improvement backlog.

The backlog lives in a `backlogs/` directory:

    backlogs/<category><NN>-<slug>.md        open items
    backlogs/done/<category><NN>-<slug>.md   completed items
    backlogs/README.md                       index (one table per category)

This script does the deterministic, error-prone parts of the workflow so an agent
(or a human) doesn't have to count sequence numbers, hand-edit the index, or forget
to flip the Status field. It is stdlib-only (Python 3.8+).

Subcommands:
    list      List items with parsed fields. Filter by --category/--status/--severity/--open.
    next      Print the next two-digit sequence number for a category (scans open + done).
    new       Create a new item file from the template with the correct next number.
    validate  Validate one item's filename, front-matter, and required sections.
    check     Validate every item + report duplicate/gap sequence numbers per category.
    done      Set Status: DONE, move to done/ (git mv when possible), rebuild the index.
    wont      Set Status: WON'T (intended), add a decision note, rebuild the index.
    index     Rebuild the "## Index" section of README.md from the files on disk.

Examples:
    python3 backlog.py list --backlogs backlogs --open
    python3 backlog.py next --backlogs backlogs c
    python3 backlog.py new --backlogs backlogs c my-slug --title "My item" --severity High
    python3 backlog.py done --backlogs backlogs c09-surface-device-errors-to-user.md
    python3 backlog.py index --backlogs backlogs
"""

import argparse
import os
import re
import shutil
import subprocess
import sys

# --- Fixed conventions -------------------------------------------------------

CATEGORIES = {
    "c": "Correctness & reliability",
    "a": "Architecture & maintainability",
    "t": "Type safety",
    "d": "Testing & CI",
    "b": "Build, deploy & config",
    "f": "Cleanup & i18n",
}
CATEGORY_ORDER = ["c", "a", "t", "d", "b", "f"]

SEVERITIES = ["High", "Medium", "Low"]
STATUSES = ["TODO", "DONE", "WON'T (intended)"]
EFFORTS = ["S", "M", "L"]

# <category><NN>-<slug>.md  e.g. c09-surface-device-errors-to-user.md
FILENAME_RE = re.compile(r"^([a-z])(\d{2})-(.+)\.md$")

REQUIRED_SECTIONS = [
    "## Problem",
    "## Evidence",
    "## Why it matters",
    "## Conservative fix",
    "## Acceptance / verification",
]

TEMPLATE = """# {id} \u2014 {title}

- **Category:** {cat} ({cat_name})
- **Severity:** {severity}
- **Status:** TODO
- **Effort (est):** {effort}

## Problem
{problem}

## Evidence
- `path/to/file.ts:LINE` \u2014 what the code does and why it is a problem.

## Why it matters
- The user-visible or maintainability impact of the problem.

## Conservative fix
- The smallest change that resolves the problem.

## Out of scope (for now)
- What is deliberately left out of this item.

## Acceptance / verification
- [ ] A concrete, checkable outcome.
"""


# --- Parsing -----------------------------------------------------------------

def parse_front_matter(text):
    """Return {key: value} for `- **Key:** value` lines anywhere in the file."""
    fields = {}
    for line in text.splitlines():
        m = re.match(r"^- \*\*(.+?):\*\*\s*(.*)$", line)
        if m:
            fields[m.group(1).strip()] = m.group(2).strip()
    return fields


def parse_title(text):
    """Return the title from the H1, i.e. the text after `# <id> \u2014 <Title>`."""
    for line in text.splitlines():
        if line.startswith("# "):
            m = re.match(r"^#\s+\S+\s+\u2014\s*(.+)$", line)
            if m:
                return m.group(1).strip()
            return line[2:].strip()
    return ""


def scan_items(backlogs_dir):
    """Scan open + done items and return a list of parsed item dicts."""
    items = []
    for sub in [".", "done"]:
        d = os.path.join(backlogs_dir, sub)
        if not os.path.isdir(d):
            continue
        for fn in sorted(os.listdir(d)):
            m = FILENAME_RE.match(fn)
            if not m:
                continue
            cat, nn, slug = m.group(1), m.group(2), m.group(3)
            path = os.path.join(d, fn)
            with open(path, encoding="utf-8") as f:
                text = f.read()
            fm = parse_front_matter(text)
            items.append({
                "id": f"{cat}{nn}",
                "category": cat,
                "num": int(nn),
                "slug": slug,
                "file": fn,
                "relpath": fn if sub == "." else os.path.join("done", fn),
                "path": path,
                "done": sub == "done",
                "title": parse_title(text),
                "severity": fm.get("Severity", ""),
                "status": fm.get("Status", ""),
                "effort": fm.get("Effort (est)", ""),
            })
    return items


def _cat_sort_key(it):
    order = CATEGORY_ORDER.index(it["category"]) if it["category"] in CATEGORY_ORDER else 99
    return (order, it["num"])


# --- Validation --------------------------------------------------------------

def validate_file(path):
    """Return a list of error strings for one item file (empty list == valid)."""
    errors = []
    fn = os.path.basename(path)
    m = FILENAME_RE.match(fn)
    if not m:
        errors.append(f"filename does not match <category><NN>-<slug>.md: {fn}")
    elif m.group(1) not in CATEGORIES:
        errors.append(f"unknown category '{m.group(1)}' in filename {fn} (valid: {list(CATEGORIES)})")

    try:
        with open(path, encoding="utf-8") as f:
            text = f.read()
    except OSError as e:
        return [f"cannot read {fn}: {e}"]

    fm = parse_front_matter(text)
    for key in ["Category", "Severity", "Status", "Effort (est)"]:
        if not fm.get(key):
            errors.append(f"missing front-matter field: {key}")
    if fm.get("Severity") and fm["Severity"] not in SEVERITIES:
        errors.append(f"invalid Severity '{fm['Severity']}' (expected one of {SEVERITIES})")
    if fm.get("Status") and fm["Status"] not in STATUSES:
        errors.append(f"invalid Status '{fm['Status']}' (expected one of {STATUSES})")
    if fm.get("Effort (est)") and fm["Effort (est)"] not in EFFORTS:
        errors.append(f"invalid Effort (est) '{fm['Effort (est)']}' (expected one of {EFFORTS})")

    for section in REQUIRED_SECTIONS:
        if section not in text:
            errors.append(f"missing section: {section}")

    # Status should agree with location (open dir vs done/).
    if m:
        in_done = os.path.basename(os.path.dirname(path)) == "done"
        if in_done and fm.get("Status") != "DONE":
            errors.append(f"in done/ but Status is '{fm.get('Status')}' (expected DONE)")
        if not in_done and fm.get("Status") == "DONE":
            errors.append(f"Status is DONE but file is not in done/")
    return errors


# --- Index -------------------------------------------------------------------

def build_index_table(items):
    lines = []
    for cat in CATEGORY_ORDER:
        cat_items = sorted((it for it in items if it["category"] == cat), key=lambda it: it["num"])
        if not cat_items:
            continue
        lines.append(f"### {cat} \u2014 {CATEGORIES[cat]}")
        lines.append("| File | Title | Severity | Effort | Status |")
        lines.append("|------|-------|----------|--------|--------|")
        for it in cat_items:
            lines.append(
                f"| [{it['id']}]({it['relpath']}) | {it['title']} "
                f"| {it['severity']} | {it['effort']} | {it['status']} |"
            )
        lines.append("")
    return "\n".join(lines).rstrip() + "\n"


def rebuild_index(backlogs_dir):
    readme = os.path.join(backlogs_dir, "README.md")
    if not os.path.exists(readme):
        print(f"error: no README.md in {backlogs_dir}", file=sys.stderr)
        return False
    items = scan_items(backlogs_dir)
    with open(readme, encoding="utf-8") as f:
        text = f.read()
    table = build_index_table(items)
    idx = text.find("## Index")
    if idx == -1:
        new_text = text.rstrip() + "\n\n## Index\n\n" + table
    else:
        new_text = text[:idx] + "## Index\n\n" + table
    with open(readme, "w", encoding="utf-8") as f:
        f.write(new_text)
    print(f"rebuilt index in {readme} ({len(items)} items)")
    return True


def _git_mv(src, dest, cwd):
    """git mv when the file is tracked (preserves rename detection); else plain move."""
    try:
        subprocess.run(["git", "mv", src, dest], check=True, cwd=cwd,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return True
    except Exception:
        shutil.move(src, dest)
        return False


def _set_status(path, status):
    with open(path, encoding="utf-8") as f:
        text = f.read()
    text, n = re.subn(r"^- \*\*Status:\*\*.*$", f"- **Status:** {status}", text, count=1, flags=re.M)
    if n == 0:
        # No Status line yet; insert after the Effort line.
        text, n = re.subn(
            r"(- \*\*Effort \(est\):\*\*.*$)",
            r"\1\n- **Status:** " + status,
            text, count=1, flags=re.M,
        )
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


# --- Commands ----------------------------------------------------------------

def cmd_list(args):
    items = scan_items(args.backlogs)
    if args.category:
        items = [it for it in items if it["category"] == args.category]
    if args.status:
        items = [it for it in items if it["status"] == args.status]
    if args.severity:
        items = [it for it in items if it["severity"] == args.severity]
    if args.open:
        items = [it for it in items if not it["done"]]
    items.sort(key=_cat_sort_key)
    if not items:
        print("(no items match)")
        return
    for it in items:
        loc = "done/" if it["done"] else ""
        print(f"{it['id']:<4} {it['severity']:<6} {it['effort']:<2} {it['status']:<18} {loc}{it['file']}  {it['title']}")


def cmd_next(args):
    items = scan_items(args.backlogs)
    nums = [it["num"] for it in items if it["category"] == args.category]
    nxt = (max(nums) + 1) if nums else 1
    print(f"{args.category}{nxt:02d}")


def cmd_new(args):
    if args.category not in CATEGORIES:
        print(f"error: unknown category '{args.category}' (valid: {list(CATEGORIES)})", file=sys.stderr)
        sys.exit(1)
    items = scan_items(args.backlogs)
    nums = [it["num"] for it in items if it["category"] == args.category]
    nxt = (max(nums) + 1) if nums else 1
    nid = f"{args.category}{nxt:02d}"
    fn = f"{nid}-{args.slug}.md"
    path = os.path.join(args.backlogs, fn)
    if os.path.exists(path):
        print(f"error: refusing to overwrite existing {path}", file=sys.stderr)
        sys.exit(1)
    content = TEMPLATE.format(
        id=nid,
        title=args.title,
        cat=args.category,
        cat_name=CATEGORIES[args.category],
        severity=args.severity,
        effort=args.effort,
        problem=args.problem or "(describe the problem)",
    )
    with open(path, "w", encoding="utf-8") as f:
        f.write(content)
    print(f"created {path}")
    print(f"next: fill in the sections, then sync the index ->  python3 {os.path.basename(__file__)} index --backlogs {args.backlogs}")


def cmd_validate(args):
    path = args.file
    if not os.path.isabs(path):
        path = os.path.join(args.backlogs, path)
    errors = validate_file(path)
    if errors:
        print(f"INVALID: {os.path.basename(path)}")
        for e in errors:
            print(f"  - {e}")
        sys.exit(1)
    print(f"OK: {os.path.basename(path)}")


def cmd_check(args):
    items = scan_items(args.backlogs)
    bad = 0
    for it in sorted(items, key=_cat_sort_key):
        errors = validate_file(it["path"])
        if errors:
            bad += 1
            print(f"INVALID: {it['relpath']}")
            for e in errors:
                print(f"  - {e}")
    # duplicate sequence numbers within a category
    seen = {}
    for it in sorted(items, key=_cat_sort_key):
        key = (it["category"], it["num"])
        if key in seen:
            print(f"DUPLICATE: {it['category']}{it['num']:02d} used by both {seen[key]} and {it['relpath']}")
            bad += 1
        else:
            seen[key] = it["relpath"]
    if bad == 0:
        print(f"OK: all {len(items)} items valid")
    else:
        print(f"\n{bad} problem(s) found")
        sys.exit(1)


def cmd_done(args):
    path = args.file
    if not os.path.isabs(path):
        path = os.path.join(args.backlogs, path)
    if not os.path.exists(path):
        print(f"error: not found: {path}", file=sys.stderr)
        sys.exit(1)
    _set_status(path, "DONE")
    done_dir = os.path.join(args.backlogs, "done")
    os.makedirs(done_dir, exist_ok=True)
    dest = os.path.join(done_dir, os.path.basename(path))
    _git_mv(path, dest, args.backlogs)
    rebuild_index(args.backlogs)
    print(f"marked done: {os.path.basename(path)} -> done/")


def cmd_wont(args):
    path = args.file
    if not os.path.isabs(path):
        path = os.path.join(args.backlogs, path)
    if not os.path.exists(path):
        print(f"error: not found: {path}", file=sys.stderr)
        sys.exit(1)
    _set_status(path, "WON'T (intended)")
    with open(path, encoding="utf-8") as f:
        text = f.read()
    if "**Decision:**" not in text:
        note = f"> **Decision:** Won't implement \u2014 {args.note}"
        text, n = re.subn(
            r"(- \*\*Effort \(est\):\*\*.*$)",
            r"\1\n\n" + note,
            text, count=1, flags=re.M,
        )
        if n == 0:
            text = text.rstrip() + "\n\n" + note + "\n"
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)
    rebuild_index(args.backlogs)
    print(f"marked won't-do: {os.path.basename(path)}")


def cmd_index(args):
    if not rebuild_index(args.backlogs):
        sys.exit(1)


# --- CLI ---------------------------------------------------------------------

def _common(p):
    p.add_argument("--backlogs", default="backlogs", help="path to the backlogs/ directory (default: backlogs)")


def main(argv=None):
    parser = argparse.ArgumentParser(prog="backlog.py", description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("list", help="list items with parsed fields")
    _common(p)
    p.add_argument("--category", help="filter by category letter (c/a/t/d/b/f)")
    p.add_argument("--status", help="filter by status (TODO/DONE/WON'T (intended))")
    p.add_argument("--severity", help="filter by severity (High/Medium/Low)")
    p.add_argument("--open", action="store_true", help="only open items (not in done/)")
    p.set_defaults(func=cmd_list)

    p = sub.add_parser("next", help="print the next sequence number for a category")
    _common(p)
    p.add_argument("category", help="category letter (c/a/t/d/b/f)")
    p.set_defaults(func=cmd_next)

    p = sub.add_parser("new", help="create a new item from the template")
    _common(p)
    p.add_argument("category", help="category letter (c/a/t/d/b/f)")
    p.add_argument("slug", help="kebab-case slug")
    p.add_argument("--title", required=True, help="item title (goes in the H1)")
    p.add_argument("--severity", default="Medium", choices=SEVERITIES)
    p.add_argument("--effort", default="M", choices=EFFORTS)
    p.add_argument("--problem", help="optional Problem text to seed the file")
    p.set_defaults(func=cmd_new)

    p = sub.add_parser("validate", help="validate one item file")
    _common(p)
    p.add_argument("file", help="item filename (relative to --backlogs) or absolute path")
    p.set_defaults(func=cmd_validate)

    p = sub.add_parser("check", help="validate all items + report sequence problems")
    _common(p)
    p.set_defaults(func=cmd_check)

    p = sub.add_parser("done", help="set Status DONE, move to done/, rebuild index")
    _common(p)
    p.add_argument("file", help="item filename (relative to --backlogs) or absolute path")
    p.set_defaults(func=cmd_done)

    p = sub.add_parser("wont", help="set Status WON'T (intended), add decision note, rebuild index")
    _common(p)
    p.add_argument("file", help="item filename (relative to --backlogs) or absolute path")
    p.add_argument("--note", default="the behavior is intended", help="short decision note")
    p.set_defaults(func=cmd_wont)

    p = sub.add_parser("index", help="rebuild the README index from the files on disk")
    _common(p)
    p.set_defaults(func=cmd_index)

    args = parser.parse_args(argv)
    args.func(args)


if __name__ == "__main__":
    main()
