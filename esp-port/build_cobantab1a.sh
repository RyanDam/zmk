#!/bin/bash
# Build ZMK for the Coban Tab 1a: BLE HID + AXS15231B 3.5" QSPI display (LVGL
# status screen) + integrated I2C touch, ESP32-S3 (N16R8) PROCPU.
#
# Usage (works from anywhere in the repo; sh or bash):
#   sh esp-port/build_cobantab1a.sh                 # full build (needs hal_espressif blobs)
#   sh esp-port/build_cobantab1a.sh --no-blobs      # compile-only, no Espressif blobs
#   sh esp-port/build_cobantab1a.sh --flash         # build, then west flash
#   sh esp-port/build_cobantab1a.sh --log           # accepted, ignored (see -l)
#
# Options:
#   --no-blobs   Add CONFIG_BUILD_ONLY_NO_BLOBS=y (compile-only; for a real
#                image fetch the blobs first: west blobs fetch hal_espressif)
#   -l, --log    Ignored: this build's OTG composite (HID + Studio RPC CDC) is
#                already at the S3's 4 IN TxFIFO limit, so a 2nd "ZMK Logging"
#                CDC never enumerates; console + shell are on UART0/P1 anyway.
#                Raise log levels via -C instead.
#   -f, --flash  Run west flash after building
#   -C, --conf <file>
#                Extra Kconfig fragment (absolute, or relative to the caller's
#                dir or the repo root), applied after the shield's conf
#   -O, --overlay <file>
#                Extra devicetree overlay, applied after the shield's own one
#   -M, --cmake-arg <arg>
#                Extra raw CMake argument (repeatable)
#   -h, --help   Show help
#
# The cobantab1a shield (overlay + keymap + conf) lives in the external config
# repo: $ZMK_CONFIG/boards/shields/cobantab1a/ (default: sibling "zmk-config").
#
# The Espressif toolchain (xtensa-esp32s3-elf) is NOT in the ZMK ARM dev image
# — install it first, see esp-port/toolchain-esp32.md. Env vars below are
# process-scoped, and -p always keeps a stale CMake cache from pinning the
# wrong toolchain variant.
set -eu

# Caller's dir: -C/-O file args resolve against it, then the repo root.
INVOKE_DIR="$(pwd)"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

# Absolute passes through; relative is tried against the caller's dir first,
# then the repo root. Fails if found in neither.
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
  -l, --log    Not supported (see header: a logging CDC won't enumerate
               on this device; console/shell are on UART0/P1 anyway)
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

# ESP32 toolchain (see esp-port/toolchain-esp32.md). Zephyr 4.4 removed the
# "espressif" toolchain variant and the local SDK lacks the Xtensa toolchain
# the S3 needs, so use the Espressif GCC via the cross-compile variant.
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
  echo "Set ZMK_CONFIG to the external config repo containing the cobantab1a shield." >&2
  exit 1
fi

BOARD=cobantab1a/esp32s3/procpu/zmk
SHIELD=cobantab1a
# BUILD_DIR_OVERRIDE lets scratch/diagnostic builds keep their own dir.
BUILD_DIR="${BUILD_DIR_OVERRIDE:-build/cobantab1a}"

EXTRA_ARGS=""
if [ "$NO_BLOBS" = yes ]; then
  EXTRA_ARGS="$EXTRA_ARGS -DCONFIG_BUILD_ONLY_NO_BLOBS=y"
fi
if [ "$DO_LOG" = yes ]; then
  echo "NOTE: --log ignored on tab1a — the merged build's OTG composite" >&2
  echo "      (HID + Studio RPC CDC) has no IN TxFIFO headroom for a" >&2
  echo "      logging CDC; console/shell are on UART0/P1. Use -C to raise" >&2
  echo "      log levels." >&2
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

# Must run before configure: the pinned zephyr rev drops SPI_LINES_QUAD, which
# the display needs (see esp-port/patches/); idempotent.
sh esp-port/apply-zephyr-patches.sh

echo "=== building ZMK for $BOARD + $SHIELD -> $BUILD_DIR (pristine) ==="
# -S means --snippet in this west version; the shield is passed as -DSHIELD.
# shellcheck disable=SC2086
# CROSS_COMPILE goes via the environment (the CMake cache var is typed PATH and
# mangles a relative prefix); TOOLCHAIN_HOME has no env fallback, so -D here.
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
