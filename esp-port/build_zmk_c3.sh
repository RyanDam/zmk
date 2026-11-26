#!/bin/bash
# Build ZMK for the ESP32-C3 SuperMini: BLE HID only, 4x4 matrix fixture on
# the GPIO header.
#
# Usage (works from anywhere in the repo; sh or bash):
#   sh esp-port/build_zmk_c3.sh                 # full build (needs hal_espressif blobs)
#   sh esp-port/build_zmk_c3.sh --no-blobs      # compile-only, no Espressif blobs
#   sh esp-port/build_zmk_c3.sh --flash         # build, then west flash
#   sh esp-port/build_zmk_c3.sh --log           # accepted, ignored (see -l)
#
# Options:
#   --no-blobs   Add CONFIG_BUILD_ONLY_NO_BLOBS=y (compile-only; for a real
#                image fetch the blobs first: west blobs fetch hal_espressif)
#   -l, --log    Ignored: zmk-usb-logging needs a USB device controller and
#                this Zephyr port has none for the C3 (only USB-Serial/JTAG,
#                already the console).
#   -f, --flash  Run west flash after building
#   -C, --conf <file>  Extra Kconfig fragment, relative to the repo root
#   -h, --help   Show help
#
# The 4x4 test keymap lives in the external config repo:
# $ZMK_CONFIG/esp32c3_matrix_fixture.keymap (default: sibling "zmk-config").
#
# The Espressif toolchain (riscv32-esp-elf) is NOT in the ZMK ARM dev image —
# install it first, see esp-port/toolchain-esp32.md. Env vars below are
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
  -l, --log    Not supported (no USB device controller on the C3 in
               this Zephyr port — logs stay on USB-Serial/JTAG)
  -f, --flash  Run west flash after building
  -h, --help   Show this help

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
# --log accepted for uniformity but ignored (and ZMK_BUILD_LOG=1 likewise).
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
    -C|--conf)
      if [ $# -lt 2 ]; then
        echo "ERROR: $1 needs an argument" >&2
        exit 2
      fi
      CONF_FILE="$2"
      shift
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
# the riscv32 toolchain the C3 needs, so use the Espressif GCC via the
# cross-compile variant.
export ZEPHYR_TOOLCHAIN_VARIANT=cross-compile
export ESPRESSIF_TOOLCHAIN_PATH="${ESPRESSIF_TOOLCHAIN_PATH:-/opt/espressif/tools}"
export CROSS_COMPILE="${CROSS_COMPILE:-riscv32-esp-elf-}"
export TOOLCHAIN_HOME="$ESPRESSIF_TOOLCHAIN_PATH/riscv32-esp-elf/bin"
# ~/.local/bin: pip --user esptool (>=5.0.2), required for ESP32 image generation
export PATH="$ESPRESSIF_TOOLCHAIN_PATH/riscv32-esp-elf/bin:$HOME/.local/bin:$PATH"

if [ ! -x "$ESPRESSIF_TOOLCHAIN_PATH/riscv32-esp-elf/bin/${CROSS_COMPILE}gcc" ]; then
  echo "ERROR: ESP32 toolchain not found at $ESPRESSIF_TOOLCHAIN_PATH/riscv32-esp-elf" >&2
  echo "Install it first — see esp-port/toolchain-esp32.md" >&2
  exit 1
fi

ZMK_CONFIG="${ZMK_CONFIG:-$(cd "$ROOT/.." && pwd)/zmk-config}"
if [ ! -d "$ZMK_CONFIG" ]; then
  echo "ERROR: ZMK config repo not found at $ZMK_CONFIG" >&2
  echo "Set ZMK_CONFIG to the external config repo containing the keymap." >&2
  exit 1
fi

BOARD=esp32c3_supermini/esp32c3/zmk
SHIELD=esp32c3_matrix_fixture
BUILD_DIR=build/c3-zmk

EXTRA_ARGS=""
if [ -n "${CONF_FILE:-}" ]; then
  EXTRA_ARGS="$EXTRA_ARGS -DEXTRA_CONF_FILE=$ROOT/$CONF_FILE"
fi
if [ "$NO_BLOBS" = yes ]; then
  EXTRA_ARGS="$EXTRA_ARGS -DCONFIG_BUILD_ONLY_NO_BLOBS=y"
fi
if [ "$DO_LOG" = yes ]; then
  echo "NOTE: --log ignored on the C3 fixture — no USB device controller" >&2
  echo "      in this Zephyr port; console/shell stay on USB-Serial/JTAG." >&2
fi

echo "=== building ZMK for $BOARD + $SHIELD -> $BUILD_DIR (pristine) ==="
# -S means --snippet in this west version; the shield is passed as -DSHIELD.
# shellcheck disable=SC2086
# CROSS_COMPILE goes via the environment (the CMake cache var is typed PATH and
# mangles a relative prefix); TOOLCHAIN_HOME has no env fallback, so -D here.
west build -b "$BOARD" -d "$BUILD_DIR" -s app -p always \
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
