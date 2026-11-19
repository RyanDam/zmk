#!/bin/sh
# Re-apply the patches this workspace carries (esp-port/patches/).
#
# Two kinds live here:
#   - the delta this workspace's Zephyr carried on the old fork pin
#     (v4.4.1+zmk-fixes), kept so the move to upstream v4.5.0 is a drop-in:
#     0004-0012, one file per area, each header naming the original commits,
#     which are also history on the v4.5.0+coban-fixes branch;
#   - ESP32 MSPI support, which no Zephyr release has yet (0002 + 0003, being
#     #121107 stacked on #117188, with its companion hal_espressif change).
#
# `west update` resets zephyr/ and modules/ to the pinned revisions, so all of
# it is re-applied after every update.
#
# Which repo a patch belongs to: a sibling <patch>.target holds the directory
# relative to this repo's root. No .target means zephyr/ (patches here are
# mostly Zephyr patches, and that keeps the common case quiet).
#
# How "already applied" is decided: each patch has a sibling <patch>.verify
# listing strings that only the patched tree contains. Patch exit codes are not
# used for that decision - patch(1) reports success for "already applied" and
# "context drifted" alike, which would silently leave the display dead.
#
# RETIRED lists patches that must NOT be present, because the pinned revision
# now has the fix and applying the old patch would be wrong. A copy that comes
# back (an old branch, a restored backup) fails the run instead of quietly
# landing a stale hunk.
#
# POSIX sh compatible. Idempotent. Run from anywhere.
#
# Usage: sh esp-port/apply-zephyr-patches.sh

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PATCHES="$ROOT/esp-port/patches"
RETIRED="$PATCHES/RETIRED"

# 1. Retired patches must not come back.

if [ -f "$RETIRED" ]; then
  while IFS= read -r name; do
    case "$name" in '' | '#'*) continue ;; esac
    if [ -e "$PATCHES/$name" ]; then
      echo "  FAIL $name is retired - the pinned revision has that fix now." >&2
      echo "       Applying it again would either do nothing or corrupt the tree." >&2
      echo "       Delete it (esp-port/patches/RETIRED says when)." >&2
      exit 1
    fi
  done <"$RETIRED"
fi

# 2. Apply what is carried.

# Files a patch touches, from its "+++ b/<path>" lines. /dev/null appears there
# for deletions, and is not a file that should ever exist in the tree.
patch_files() {
  sed -n 's/^+++ b\/\(.*\)$/\1/p' "$1" | grep -v '^/dev/null$' | sort -u
}

# Added *empty* files, which the two functions above cannot see.
#
# git records a new empty file as a section with `new file mode` and an index
# line and NOTHING else: no ---/+++ pair, no @@ hunk. patch(1) has no work to
# do for such a section and creates nothing, and `+++ b/` never names it, so a
# content probe can pass while the file is simply absent. Zephyr boards ship
# empty files (boards/boardsource/blok/board.cmake is one), so this is real:
# the first run of this patch set silently produced a blok board with no
# board.cmake. Materialise them before applying, and demand they exist after.
#
# The path is taken from the "diff --git a/P b/P" line, and only when the a and
# b sides really match (a rename is not this shape).
empty_added_files() {
  awk '
    function flush() {
      if (path != "" && newfile && !hunk && !deleted) print path
    }
    function sym(p,  n) {              # "P b/P" -> P, else ""
      n = int((length(p) - 3) / 2)
      if (n < 1) return ""
      if (substr(p, n + 1, 3) != " b/") return ""
      if (substr(p, n + 4) != substr(p, 1, n)) return ""
      return substr(p, 1, n)
    }
    /^diff --git a\// {
      flush()
      raw = $0; sub(/^diff --git a\//, "", raw)
      path = sym(raw); newfile = 0; hunk = 0; deleted = 0
      next
    }
    /^\+\+\+ b\// { if ($0 != "+++ b/dev/null") hunk = 1; next }
    /^new file mode / { newfile = 1; next }
    /^deleted file mode / { deleted = 1; next }
    /^@@ / { hunk = 1; next }
    END { flush() }
  ' "$1" | sort -u
}

# Creates the added-empty-file holes patch(1) leaves behind. Idempotent.
materialise_empty_files() {
  repo="$1"
  p="$2"
  for f in $(empty_added_files "$p"); do
    if [ ! -e "$repo/$f" ]; then
      mkdir -p "$(dirname "$repo/$f")"
      : >"$repo/$f"
    fi
  done
}

# Every file the patch declares must be in the tree afterwards. Existence is a
# different fact from content, and the probes only check content.
files_present() {
  repo="$1"
  p="$2"
  for f in $(patch_files "$p"; empty_added_files "$p"); do
    if [ ! -e "$repo/$f" ]; then
      return 1
    fi
  done
  return 0
}

# patch(1) leaves <file>.orig/.rej when a hunk lands with an offset or fuzz,
# which every hunk here does after the first west update moves the base. A
# successful verify proves the content is right, so the backups are litter.
drop_artifacts() {
  repo="$1"
  p="$2"
  for f in $(patch_files "$p"); do
    rm -f "$repo/$f.orig" "$repo/$f.rej"
  done
}

# 0 = the fix is present (every probe satisfied), 1 = it is not.
#
# A probe line is a literal string that must be found in a file the patch
# touches. A line starting with '!' is the opposite: a string the fix REMOVES,
# which must be gone. Carried reverts add nothing, so absence is the only
# evidence they are there - pair one with a present-in-both-states anchor line
# so a renamed or restructured file still fails instead of verifying by luck.
fix_present() {
  repo="$1"
  p="$2"
  files=$(patch_files "$p")
  while IFS= read -r want; do
    case "$want" in '' | '#'*) continue ;; esac
    want_absent=no
    if [ "${want#!}" != "$want" ]; then
      want_absent=yes
      want="${want#!}"
    fi
    found=no
    for f in $files; do
      if [ -f "$repo/$f" ] && grep -qF -e "$want" "$repo/$f"; then
        found=yes
        break
      fi
    done
    if [ "$want_absent" = yes ]; then
      if [ "$found" = yes ]; then
        return 1
      fi
      continue
    fi
    if [ "$found" = no ]; then
      return 1
    fi
  done <"$p.verify"
  return 0
}

applied=0
skipped=0

for p in "$PATCHES"/*.patch; do
  [ -e "$p" ] || continue
  name="$(basename "$p")"

  if [ ! -f "$p.verify" ]; then
    echo "  FAIL $name has no $name.verify - refusing to guess whether it is applied" >&2
    exit 1
  fi

  if [ -f "$p.target" ]; then
    rel="$(sed -n '1p' "$p.target")"
  else
    rel="zephyr"
  fi
  dir="$ROOT/$rel"

  # -e, not -d: a git worktree has .git as a file, and this must be able to
  # verify itself against one (that is how the patch set is tested).
  if [ ! -e "$dir/.git" ]; then
    echo "  FAIL $name targets $rel, which is not a git repo (run west update first)" >&2
    exit 1
  fi

  rev="$(git -C "$dir" rev-parse --short HEAD)"

  if fix_present "$dir" "$p" && files_present "$dir" "$p"; then
    drop_artifacts "$dir" "$p"
    echo "  ok   $name (already applied, $rel $rev)"
    skipped=$((skipped + 1))
    continue
  fi

  # The empty-file hole has to be filled around patch(1), not by it.
  materialise_empty_files "$dir" "$p"

  # Forward-only (-N/--forward): never interactively reverses, never blocks a build.
  patch -p1 --forward -N -s -d "$dir" <"$p" >/dev/null 2>&1 || true

  if fix_present "$dir" "$p" && files_present "$dir" "$p"; then
    drop_artifacts "$dir" "$p"
    echo "  ok   $name (applied to $rel $rev)"
    applied=$((applied + 1))
  else
    echo "  FAIL $name did not apply: a probe string is still missing, or a file" >&2
    echo "       the patch declares is not in the tree ($rel $rev)" >&2
    echo "       The pinned rev has probably moved. Check by hand:" >&2
    echo "         git -C $rel log --oneline -- $(patch_files "$p" | tr '\n' ' ')" >&2
    echo "       Then rebase the patch, or move it to RETIRED if the fix is upstream." >&2
    exit 1
  fi
done

if [ "$applied" -eq 0 ] && [ "$skipped" -eq 0 ]; then
  echo "  ok   no carried patches"
fi
