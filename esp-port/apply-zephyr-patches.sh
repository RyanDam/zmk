#!/bin/sh
# Re-apply the patches this workspace carries (esp-port/patches/).
#
# Two kinds live here:
#   - fixes the pinned Zephyr predates (none right now: 4.5 has the ESP32 quad
#     line-mode fix that 4.4.1 needed);
#   - ESP32 MSPI support, which no Zephyr release has yet (#121107 + #117188,
#     with its companion hal_espressif change).
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

# Files a patch touches, from its "+++ b/<path>" lines.
patch_files() {
  sed -n 's/^+++ b\/\(.*\)$/\1/p' "$1" | sort -u
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

# 0 = the fix is present (every probe string found), 1 = it is not.
fix_present() {
  repo="$1"
  p="$2"
  files=$(patch_files "$p")
  while IFS= read -r want; do
    case "$want" in '' | '#'*) continue ;; esac
    found=no
    for f in $files; do
      if [ -f "$repo/$f" ] && grep -qF -e "$want" "$repo/$f"; then
        found=yes
        break
      fi
    done
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

  if [ ! -d "$dir/.git" ]; then
    echo "  FAIL $name targets $rel, which is not a git repo (run west update first)" >&2
    exit 1
  fi

  rev="$(git -C "$dir" rev-parse --short HEAD)"

  if fix_present "$dir" "$p"; then
    drop_artifacts "$dir" "$p"
    echo "  ok   $name (already applied, $rel $rev)"
    skipped=$((skipped + 1))
    continue
  fi

  # Forward-only (-N/--forward): never interactively reverses, never blocks a build.
  patch -p1 --forward -N -s -d "$dir" <"$p" >/dev/null 2>&1 || true

  if fix_present "$dir" "$p"; then
    drop_artifacts "$dir" "$p"
    echo "  ok   $name (applied to $rel $rev)"
    applied=$((applied + 1))
  else
    echo "  FAIL $name did not apply, and the fix is still missing ($rel $rev)" >&2
    echo "       The pinned rev has probably moved. Check by hand:" >&2
    echo "         git -C $rel log --oneline -- $(patch_files "$p" | tr '\n' ' ')" >&2
    echo "       Then rebase the patch, or move it to RETIRED if the fix is upstream." >&2
    exit 1
  fi
done

if [ "$applied" -eq 0 ] && [ "$skipped" -eq 0 ]; then
  echo "  ok   no carried patches"
fi
