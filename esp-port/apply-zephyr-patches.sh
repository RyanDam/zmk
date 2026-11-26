#!/bin/sh
# Re-apply the patches this workspace carries (esp-port/patches/) after every
# `west update`, which resets zephyr/ and modules/ to the pinned revisions.
# POSIX sh. Idempotent. Run from anywhere.
#
# Usage: sh esp-port/apply-zephyr-patches.sh
#
# Sibling files per patch:
#   <patch>.target  dir to patch, relative to this repo's root (default zephyr/)
#   <patch>.verify  strings only the patched tree contains (see fix_present).
#                   patch(1) exit codes cannot decide "already applied" — it
#                   reports success for drifted context too, which would
#                   silently leave the display dead — so the probes are the
#                   only evidence.
#   RETIRED         patches that must NOT exist (the pinned revision has the
#                   fix); a copy coming back (old branch, restored backup)
#                   fails the run instead of landing a stale hunk.

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PATCHES="$ROOT/esp-port/patches"
RETIRED="$PATCHES/RETIRED"

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

# Files a patch touches, from its "+++ b/<path>" lines; /dev/null appears
# there for deletions and is never a real file.
patch_files() {
  sed -n 's/^+++ b\/\(.*\)$/\1/p' "$1" | grep -v '^/dev/null$' | sort -u
}

# Added *empty* files, which neither probe above can see: git records them as
# a `new file mode` line with no ---/+++ pair and no hunk, patch(1) creates
# nothing for them, and `+++ b/` never names them — a content probe can pass
# while the file is simply absent (Zephyr boards ship empty files).
# Materialise them before applying, and demand they exist after.
# The path comes from the "diff --git a/P b/P" line, only when the a and b
# sides really match (a rename is not this shape).
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

# Every file the patch declares must be in the tree afterwards: the probes
# check content, not existence.
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

# patch(1) leaves .orig/.rej when a hunk lands with offset or fuzz (the norm
# once west update moves the base); the probes prove content, so drop the litter.
drop_artifacts() {
  repo="$1"
  p="$2"
  for f in $(patch_files "$p"); do
    rm -f "$repo/$f.orig" "$repo/$f.rej"
  done
}

# 0 = the fix is present (every probe satisfied), 1 = it is not.
#
# A probe line is a literal string that must appear in a file the patch
# touches. A leading '!' inverts it: a string the fix REMOVES, which must be
# gone — carried reverts add nothing, so absence is their only evidence.
# Pair a '!' with a present-in-both-states anchor line so a renamed or
# restructured file fails instead of verifying by luck.
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

  # -e, not -d: a git worktree has .git as a file (that is how the patch set
  # is tested).
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

  materialise_empty_files "$dir" "$p"

  # Forward-only (-N/--forward): never reverses, never blocks a build.
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
