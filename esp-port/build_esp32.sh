#!/bin/bash
# Build the ESP32-C3 / ESP32-S3 Zephyr bring-up samples (Phase 0).
#
# Usage (works from anywhere in the repo):
#   bash esp-port/build_esp32.sh              # build C3 + S3
#   bash esp-port/build_esp32.sh c3           # build C3 only
#   bash esp-port/build_esp32.sh s3           # build S3 only
#   bash esp-port/build_esp32.sh c3 flash     # build C3, then west flash it
#
# The ESP32 toolchain (Espressif riscv32-esp-elf + xtensa-esp32s3-elf) is NOT
# in the ZMK ARM dev image. Install it first — see esp-port/toolchain-esp32.md.
# This script sets the toolchain env vars for you (scoped to this process only,
# so it never affects the ARM ZMK builds), and uses --pristine so a stale
# CMake cache can't pin the wrong toolchain variant.

# POSIX sh compatible (works under sh/dash and bash). No pipelines are used,
# so "set -o pipefail" is not needed.
set -eu

# Run from the workspace root (parent of esp-port/) so the sample path and the
# build/ dir resolve correctly, regardless of where the script is invoked.
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

# --- ESP32 toolchain (see esp-port/toolchain-esp32.md) ---
export ZEPHYR_TOOLCHAIN_VARIANT=espressif
export ESPRESSIF_TOOLCHAIN_PATH="${ESPRESSIF_TOOLCHAIN_PATH:-/opt/espressif/tools}"

if [ ! -d "$ESPRESSIF_TOOLCHAIN_PATH" ]; then
  echo "ERROR: ESP32 toolchain not found at $ESPRESSIF_TOOLCHAIN_PATH" >&2
  echo "Install it first — see esp-port/toolchain-esp32.md" >&2
  exit 1
fi

TARGET="${1:-all}"
DO_FLASH="${2:-}"

C3_BOARD=esp32c3_devkitc/esp32c3
C3_DIR=build/c3-hello
S3_BOARD=esp32s3_devkitc/esp32s3/procpu
S3_DIR=build/s3-hello

build() { # $1=board  $2=build dir
  # -p always = full clean rebuild, so a stale CMake cache can never pin the
  # wrong toolchain variant. Change to "-p auto" for incremental rebuilds once
  # the build dir is known-good.
  echo "=== building $1 -> $2 (pristine) ==="
  west build -b "$1" -d "$2" -p always zephyr/samples/hello_world
}

flash() { # $1=build dir
  echo "=== flashing $1 ==="
  west flash -d "$1"
}

case "$TARGET" in
  c3)
    build "$C3_BOARD" "$C3_DIR"
    if [ "$DO_FLASH" = "flash" ]; then flash "$C3_DIR"; fi
    ;;
  s3)
    build "$S3_BOARD" "$S3_DIR"
    if [ "$DO_FLASH" = "flash" ]; then flash "$S3_DIR"; fi
    ;;
  all)
    build "$C3_BOARD" "$C3_DIR"
    build "$S3_BOARD" "$S3_DIR"
    if [ "$DO_FLASH" = "flash" ]; then flash "$C3_DIR"; flash "$S3_DIR"; fi
    ;;
  *)
    echo "usage: $(basename "$0") [c3|s3|all] [flash]" >&2
    exit 2
    ;;
esac

echo
echo "Done. Next steps:"
echo "  monitor:  west monitor -d build/c3-hello    (or build/s3-hello)"
echo "  reflash:  west flash   -d build/c3-hello    (or build/s3-hello)"
