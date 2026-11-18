#!/bin/bash
# Re-apply the Zephyr patches this workspace carries (esp-port/patches/).
#
# ZMK pins zephyr to v4.4.1+zmk-fixes, which predates some fixes the Coban Tab
# 1a display needs, so the fixes are carried here and re-applied after every
# `west update` (which resets zephyr/ to the pinned rev).
#
# How "already applied" is decided: each patch has a sibling .patch.verify
# listing strings that only the fixed code contains. Patch exit codes are not
# used for that decision — GNU patch reports success for both "already applied"
# and "context drifted", which would silently leave the display dead.
#
# POSIX sh compatible. Idempotent. Run from anywhere.
#
# Usage: sh esp-port/apply-zephyr-patches.sh

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ZEPHYR="$ROOT/zephyr"
PATCHES="$ROOT/esp-port/patches"

if [ ! -d "$ZEPHYR/.git" ]; then
  echo "apply-zephyr-patches: no zephyr git repo at $ZEPHYR (run west update first)" >&2
  exit 1
fi

rev="$(git -C "$ZEPHYR" rev-parse --short HEAD)"

# Files a patch touches, from its "+++ b/<path>" lines.
patch_files() {
  sed -n 's/^+++ b\/\(.*\)$/\1/p' "$1" | sort -u
}

# 0 = the fix is present (every probe string found), 1 = it is not.
fix_present() {
  files=$(patch_files "$1")
  while IFS= read -r want; do
    case "$want" in '' | '#'*) continue ;; esac
    found=no
    for f in $files; do
      if [ -f "$ZEPHYR/$f" ] && grep -qF -e "$want" "$ZEPHYR/$f"; then
        found=yes
        break
      fi
    done
    if [ "$found" = no ]; then
      return 1
    fi
  done <"$1.verify"
  return 0
}

applied=0
skipped=0

for p in "$PATCHES"/*.patch; do
  [ -e "$p" ] || continue
  name="$(basename "$p")"

  if [ ! -f "$p.verify" ]; then
    echo "  FAIL $name has no $name.verify — refusing to guess whether it is applied" >&2
    exit 1
  fi

  if fix_present "$p"; then
    echo "  ok   $name (already applied, zephyr $rev)"
    skipped=$((skipped + 1))
    continue
  fi

  # Forward-only (-N/--forward): never interactively reverses, never blocks a build.
  patch -p1 --forward -N -s -d "$ZEPHYR" <"$p" >/dev/null 2>&1 || true

  if fix_present "$p"; then
    echo "  ok   $name (applied to zephyr $rev)"
    applied=$((applied + 1))
  else
    echo "  FAIL $name did not apply, and the fix is still missing (zephyr $rev)" >&2
    echo "       The pinned rev has probably moved. Check by hand:" >&2
    echo "         git -C zephyr log --oneline -- $(patch_files "$p" | tr '\n' ' ')" >&2
    echo "       Then rebase the patch, or delete it (and its .verify) if the fix is upstream." >&2
    exit 1
  fi
done

if [ "$applied" -eq 0 ] && [ "$skipped" -eq 0 ]; then
  echo "  ok   no carried patches"
fi
