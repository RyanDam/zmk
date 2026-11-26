#!/bin/bash
# Build ZMK for the ESP32-S3 SuperMini: BLE HID only, 4x4 matrix fixture on
# the GPIO header, PROCPU (single-core).
#
# Usage (works from anywhere in the repo; sh or bash):
#   sh esp-port/build_zmk_s3.sh                 # full build (needs hal_espressif blobs)
#   sh esp-port/build_zmk_s3.sh --no-blobs      # compile-only, no Espressif blobs
#   sh esp-port/build_zmk_s3.sh --flash         # build, then west flash
#   sh esp-port/build_zmk_s3.sh --log           # build with verbose logging
#
# Options:
#   --no-blobs   Add CONFIG_BUILD_ONLY_NO_BLOBS=y (compile-only; for a real
#                image fetch the blobs first: west blobs fetch hal_espressif)
#   -l, --log    zmk-usb-logging snippet: a dedicated "ZMK Logging" CDC;
#                console + shell move to it. Also ZMK_BUILD_LOG=1.
#   -f, --flash  Run west flash after building
#   -h, --help   Show help
#
# The 4x4 test keymap lives in the external config repo:
# $ZMK_CONFIG/esp32s3_matrix_fixture.keymap (default: sibling "zmk-config").
#
# The Espressif toolchain (xtensa-esp32s3-elf) is NOT in the ZMK ARM dev image
# — install it first, see esp-port/toolchain-esp32.md. Env vars below are
# process-scoped, and -p always keeps a stale CMake cache from pinning the
# wrong toolchain variant.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

usage() {
  cat <<EOF
Usage: $(basename "$0") [options]

Options:
  --no-blobs   Compile-only build (CONFIG_BUILD_ONLY_NO_BLOBS=y)
  -l, --log    Add the zmk-usb-logging snippet ("ZMK Logging" CDC; console
               + shell move to it). Env fallback: ZMK_BUILD_LOG=1
  -f, --flash  Run west flash after building
  -h, --help   Show help

Environment:
  ZMK_CONFIG               External ZMK config repo (default: sibling "zmk-config")
  ESPRESSIF_TOOLCHAIN_PATH Toolchain dir (default: /opt/espressif/tools)

Examples:
  $(basename "$0") --no-blobs
  $(basename "$0") --flash
EOF
}

DO_FLASH=no
NO_BLOBS=no
# --log flag or ZMK_BUILD_LOG=1 (exported by the workspace build.sh --log).
DO_LOG="${ZMK_BUILD_LOG:-no}"
case "$DO_LOG" in 1|yes|y|true) DO_LOG=yes ;; *) DO_LOG=no ;; esac

while [ $# -gt 0 ]; do
  case "$1" in
    --no-blobs)
      NO_BLOBS=yes
      ;;
    -l|--log)
      DO_LOG=yes
      ;;
    -f|--flash)
      DO_FLASH=yes
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
  shift
done

# --- ESP32 toolchain (see esp-port/toolchain-esp32.md) ---
# Zephyr 4.4 removed the "espressif" toolchain variant and the local SDK lacks
# the Xtensa toolchain the S3 needs, so use the Espressif GCC via the
# cross-compile variant.
export ZEPHYR_TOOLCHAIN_VARIANT=cross-compile
export ESPRESSIF_TOOLCHAIN_PATH="${ESPRESSIF_TOOLCHAIN_PATH:-/opt/espressif/tools}"
export CROSS_COMPILE="${CROSS_COMPILE:-xtensa-esp32s3-elf-}"
export TOOLCHAIN_HOME="$ESPRESSIF_TOOLCHAIN_PATH/xtensa-esp32s3-elf/bin"
# ~/.local/bin: pip --user esptool (>=5.0.2), required for ESP32 image generation
export PATH="$ESPRESSIF_TOOLCHAIN_PATH/xtensa-esp32s3-elf/bin:$HOME/.local/bin:$PATH"

if [ ! -x "$ESPRESSIF_TOOLCHAIN_PATH/xtensa-esp32s3-elf/bin/${CROSS_COMPILE}gcc" ]; then
  echo "ERROR: ESP32 toolchain not found at $ESPRESSIF_TOOLCHAIN_PATH/xtensa-esp32s3-elf" >&2
  echo "Install it first — see esp-port/toolchain-esp32.md" >&2
  exit 1
fi

ZMK_CONFIG="${ZMK_CONFIG:-$(cd "$ROOT/.." && pwd)/zmk-config}"
if [ ! -d "$ZMK_CONFIG" ]; then
  echo "ERROR: ZMK config repo not found at $ZMK_CONFIG" >&2
  echo "Set ZMK_CONFIG to the external config repo containing the keymap." >&2
  exit 1
fi

BOARD=esp32s3_supermini/esp32s3/procpu/zmk
SHIELD=esp32s3_matrix_fixture
BUILD_DIR=build/s3-zmk

EXTRA_ARGS=""
if [ "$NO_BLOBS" = yes ]; then
  EXTRA_ARGS="$EXTRA_ARGS -DCONFIG_BUILD_ONLY_NO_BLOBS=y"
fi
SNIPPETS=""
if [ "$DO_LOG" = yes ]; then
  # The esp32s3-usb-otg snippet provides the zephyr_udc0 label and MUST lead:
  # DTS resolves label references in stream order.
  SNIPPETS="-S esp32s3-usb-otg -S zmk-usb-logging"
fi

echo "=== building ZMK for $BOARD + $SHIELD -> $BUILD_DIR (pristine) ==="
# -S means --snippet in this west version; the shield is passed as -DSHIELD.
# shellcheck disable=SC2086
# CROSS_COMPILE goes via the environment (the CMake cache var is typed PATH and
# mangles a relative prefix); TOOLCHAIN_HOME has no env fallback, so -D here.
west build -b "$BOARD" -d "$BUILD_DIR" -s app -p always $SNIPPETS \
  -- -DZMK_CONFIG="$ZMK_CONFIG" -DSHIELD="$SHIELD" \
     -DTOOLCHAIN_HOME="$TOOLCHAIN_HOME" $EXTRA_ARGS

if [ "$DO_FLASH" = yes ]; then
  echo "=== flashing $BUILD_DIR ==="
  west flash -d "$BUILD_DIR"
fi

echo
echo "Done. Next steps:"
echo "  monitor: west monitor -d $BUILD_DIR"
echo "  reflash: west flash   -d $BUILD_DIR"
