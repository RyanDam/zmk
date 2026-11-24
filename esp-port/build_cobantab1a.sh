#!/bin/bash
# Build the ZMK firmware for the Coban Tab 1a (Phase 9 / b05): BLE HID +
# AXS15231B 3.5" QSPI display (LVGL status screen) + integrated I2C touch, on
# the ESP32-S3 (N16R8, 16 MB flash + 8 MB octal PSRAM). PROCPU (single-core).
#
# Usage (works from anywhere in the repo; sh or bash):
#   sh esp-port/build_cobantab1a.sh                 # full build (needs hal_espressif blobs)
#   sh esp-port/build_cobantab1a.sh --no-blobs      # compile-only, no Espressif blobs
#   sh esp-port/build_cobantab1a.sh --flash         # build, then west flash
#
# Options:
#   --no-blobs   Add CONFIG_BUILD_ONLY_NO_BLOBS=y (compile-only; the Espressif
#                proprietary blobs are not needed. Fetch them for a real image:
#                west blobs fetch hal_espressif)
#   -f, --flash  Run west flash after building
#   -C, --conf <file>
#                Extra Kconfig fragment (relative to the repo root, or
#                absolute) for a bring-up build, e.g.
#                esp-port/bringup/init-only.conf. Fragments are applied after
#                the shield's conf, so they can switch instruments on and off.
#   -O, --overlay <file>
#                Extra devicetree overlay, applied after the shield's own one
#                (e.g. esp-port/bringup/spi-path.overlay, which puts the panel
#                back on the SPI controller instead of the MSPI one)
#   -M, --cmake-arg <arg>
#                Extra raw CMake argument for the build (repeatable), e.g.
#                -M "-DEXTRA_CFLAGS=-DCONFIG_BT_LE_50_FEATURES_SUPPORTED=0"
#                for preprocessor-level bring-up experiments
#   -h, --help   Show help
#
# The cobantab1a shield (overlay + keymap + conf) lives in the external ZMK
# config repo (sibling of this repo by default): $ZMK_CONFIG/boards/shields/
# cobantab1a/. Override the location with the ZMK_CONFIG environment variable.
#
# The ESP32 toolchain (Espressif xtensa-esp32s3-elf) is NOT in the ZMK ARM dev
# image. Install it first — see esp-port/toolchain-esp32.md. This script sets
# the toolchain env vars for you (scoped to this process only), and uses
# -p always so a stale CMake cache can't pin the wrong toolchain variant.

# POSIX sh compatible (works under sh/dash and bash). No pipelines are used,
# so "set -o pipefail" is not needed.
set -eu

# Where the caller was standing, kept because -C/-O arguments get typed relative
# to it as often as they get typed relative to the repo root.
INVOKE_DIR="$(pwd)"

# Run from the workspace root (parent of esp-port/) so the paths resolve
# correctly, regardless of where the script is invoked.
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

# Resolve a file argument to an absolute path: absolute passes through, a
# relative path is tried against the caller's directory first and the repo root
# second, so both
#   cd zmk && sh esp-port/build_cobantab1a.sh -C esp-port/bringup/x.conf
#   sh zmk/esp-port/build_cobantab1a.sh -C zmk/esp-port/bringup/x.conf
# work. Fails if the file is in neither place.
resolve_file() {
  case "$1" in
    /*)
      if [ -f "$1" ]; then
        printf '%s\n' "$1"
        return 0
      fi
      ;;
    *)
      if [ -f "$INVOKE_DIR/$1" ]; then
        printf '%s\n' "$INVOKE_DIR/$1"
        return 0
      fi
      if [ -f "$ROOT/$1" ]; then
        printf '%s\n' "$ROOT/$1"
        return 0
      fi
      ;;
  esac
  return 1
}

usage() {
  cat <<EOF
Usage: $(basename "$0") [options]

Options:
  --no-blobs   Compile-only build (CONFIG_BUILD_ONLY_NO_BLOBS=y)
  -f, --flash  Run west flash after building
  -C, --conf <file>
               Extra Kconfig fragment for a bring-up build, relative to the
               repo root or absolute (e.g. esp-port/bringup/init-only.conf)
  -O, --overlay <file>
               Extra devicetree overlay, applied after the shield's own
               overlay (e.g. esp-port/bringup/spi-path.overlay)
  -h, --help   Show help

Environment:
  ZMK_CONFIG               External ZMK config repo (default: sibling "zmk-config")
  ESPRESSIF_TOOLCHAIN_PATH Toolchain dir (default: /opt/espressif/tools)

Examples:
  $(basename "$0") --no-blobs
  $(basename "$0") --flash
  $(basename "$0") -C esp-port/bringup/init-only.conf
  $(basename "$0") -C esp-port/bringup/spi-path.conf -O esp-port/bringup/spi-path.overlay
EOF
}

DO_FLASH=no
NO_BLOBS=no
CONF_FILE=""
OVERLAY_FILE=""
CMAKE_ARGS=""

while [ $# -gt 0 ]; do
  case "$1" in
    --no-blobs)
      NO_BLOBS=yes
      ;;
    -f|--flash)
      DO_FLASH=yes
      ;;
    -C|--conf)
      if [ $# -lt 2 ]; then
        echo "ERROR: $1 needs a file argument" >&2
        usage >&2
        exit 2
      fi
      CONF_FILE="$2"
      shift
      ;;
    -O|--overlay)
      if [ $# -lt 2 ]; then
        echo "ERROR: $1 needs a file argument" >&2
        usage >&2
        exit 2
      fi
      OVERLAY_FILE="$2"
      shift
      ;;
    -M|--cmake-arg)
      if [ $# -lt 2 ]; then
        echo "ERROR: $1 needs an argument" >&2
        usage >&2
        exit 2
      fi
      CMAKE_ARGS="$CMAKE_ARGS $2"
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
# Zephyr 4.4 removed the legacy "espressif" toolchain variant (upstream
# commit "cmake: remove deprecated espressif toolchain variant"). Espressif
# targets now build with the Zephyr SDK (which bundles the Xtensa/RISC-V
# toolchains) or with a generic cross-compile toolchain. The Zephyr SDK
# installed here lacks the Xtensa toolchain the S3 needs, so this script
# uses the Espressif GCC toolchain via the cross-compile variant.
export ZEPHYR_TOOLCHAIN_VARIANT=cross-compile
export ESPRESSIF_TOOLCHAIN_PATH="${ESPRESSIF_TOOLCHAIN_PATH:-/opt/espressif/tools}"
export CROSS_COMPILE="${CROSS_COMPILE:-xtensa-esp32s3-elf-}"
export TOOLCHAIN_HOME="$ESPRESSIF_TOOLCHAIN_PATH/xtensa-esp32s3-elf/bin"
# ~/.local/bin for a pip --user esptool (required by the ESP32 SOC cmake for
# image generation: pip3 install --user --break-system-packages 'esptool>=5.0.2')
export PATH="$ESPRESSIF_TOOLCHAIN_PATH/xtensa-esp32s3-elf/bin:$HOME/.local/bin:$PATH"

if [ ! -x "$ESPRESSIF_TOOLCHAIN_PATH/xtensa-esp32s3-elf/bin/${CROSS_COMPILE}gcc" ]; then
  echo "ERROR: ESP32 toolchain not found at $ESPRESSIF_TOOLCHAIN_PATH/xtensa-esp32s3-elf" >&2
  echo "Install it first — see esp-port/toolchain-esp32.md" >&2
  exit 1
fi

ZMK_CONFIG="${ZMK_CONFIG:-$(cd "$ROOT/.." && pwd)/zmk-config}"
if [ ! -d "$ZMK_CONFIG" ]; then
  echo "ERROR: ZMK config repo not found at $ZMK_CONFIG" >&2
  echo "Set ZMK_CONFIG to the external config repo containing the cobantab1a shield." >&2
  exit 1
fi

BOARD=cobantab1a/esp32s3/procpu/zmk
SHIELD=cobantab1a
BUILD_DIR=build/cobantab1a

EXTRA_ARGS=""
if [ "$NO_BLOBS" = yes ]; then
  EXTRA_ARGS="$EXTRA_ARGS -DCONFIG_BUILD_ONLY_NO_BLOBS=y"
fi

if [ -n "$CONF_FILE" ]; then
  if ! CONF_FILE="$(resolve_file "$CONF_FILE")"; then
    echo "ERROR: Kconfig fragment not found: $CONF_FILE (looked in $INVOKE_DIR and $ROOT)" >&2
    exit 1
  fi
  EXTRA_ARGS="$EXTRA_ARGS -DEXTRA_CONF_FILE=$CONF_FILE"
fi

if [ -n "$OVERLAY_FILE" ]; then
  if ! OVERLAY_FILE="$(resolve_file "$OVERLAY_FILE")"; then
    echo "ERROR: devicetree overlay not found: $OVERLAY_FILE (looked in $INVOKE_DIR and $ROOT)" >&2
    exit 1
  fi
  EXTRA_ARGS="$EXTRA_ARGS -DEXTRA_DTC_OVERLAY_FILE=$OVERLAY_FILE"
fi

# The display needs SPI_LINES_QUAD to reach the wire; the pinned zephyr rev
# drops it (see esp-port/patches/). Cheap and idempotent, so the display never
# silently goes dark after a `west update` that skipped setup-dev.sh.
sh esp-port/apply-zephyr-patches.sh

echo "=== building ZMK for $BOARD + $SHIELD -> $BUILD_DIR (pristine) ==="
# Note: in this west version -S means --snippet; the shield is passed as
# -DSHIELD (same as esp-port/build_zmk_s3.sh).
# shellcheck disable=SC2086
# CROSS_COMPILE is passed via the environment (the CMake cache variable is
# typed PATH and mangles a relative prefix); TOOLCHAIN_HOME has no env
# fallback, so it goes on the command line.
west build -b "$BOARD" -d "$BUILD_DIR" -s app -p always \
  -- -DZMK_CONFIG="$ZMK_CONFIG" -DSHIELD="$SHIELD" \
     -DTOOLCHAIN_HOME="$TOOLCHAIN_HOME" $EXTRA_ARGS $CMAKE_ARGS

if [ "$DO_FLASH" = yes ]; then
  echo "=== flashing $BUILD_DIR ==="
  west flash -d "$BUILD_DIR"
fi

echo
echo "Done. Next steps:"
echo "  monitor: west monitor -d $BUILD_DIR"
echo "  reflash: west flash   -d $BUILD_DIR"
