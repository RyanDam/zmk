#!/bin/bash
# Build the ESP32-C3 / ESP32-S3 Zephyr bring-up samples (Phase 0).
#
# Usage (works from anywhere in the repo; sh or bash):
#   sh esp-port/build_esp32.sh                                   # build C3 + S3 (hello_world)
#   sh esp-port/build_esp32.sh --board c3                        # build C3 only
#   sh esp-port/build_esp32.sh --board s3 --flash                # build S3, then west flash it
#   sh esp-port/build_esp32.sh --board c3 --sample philosophers --flash
#   sh esp-port/build_esp32.sh --log                            # verbose logging
#
# Options:
#   -b, --board <c3|s3|all>            Board to build (default: all)
#   -s, --sample <hello|philosophers>  Sample to build (default: hello = hello_world)
#   -f, --flash                        Run west flash after building
#   -l, --log                          Not supported here: it means the
#                                      zmk-usb-logging snippet, which lives
#                                      in the ZMK app — these plain Zephyr
#                                      samples have no ZMK to apply it to.
#                                      Use --log on build_zmk_c3/s3.sh.
#   -h, --help                         Show help
#
# The ESP32 toolchain (Espressif riscv32-esp-elf + xtensa-esp32s3-elf) is NOT
# in the ZMK ARM dev image. Install it first — see esp-port/toolchain-esp32.md.
# This script sets the toolchain env vars for you (scoped to this process only,
# so it never affects the ARM ZMK builds), and uses -p always so a stale CMake
# cache can't pin the wrong toolchain variant.
#
# The build always applies esp-port/fixtures/usb-console.overlay, which routes
# the Zephyr console to the built-in USB Serial/JTAG port. The board default
# is UART0, which the SuperMini does not bridge to USB, so without the overlay
# the monitor shows nothing.

# POSIX sh compatible (works under sh/dash and bash). No pipelines are used,
# so "set -o pipefail" is not needed.
set -eu

# Run from the workspace root (parent of esp-port/) so the sample path and the
# build/ dir resolve correctly, regardless of where the script is invoked.
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

usage() {
  cat <<EOF
Usage: $(basename "$0") [options]

Options:
  -b, --board <c3|s3|all>            Board to build (default: all)
  -s, --sample <hello|philosophers>  Sample to build (default: hello = hello_world)
  -f, --flash                        Run west flash after building
  -l, --log                          Not supported for samples (the
                                     zmk-usb-logging snippet needs the ZMK
                                     app; see build_zmk_c3.sh / _s3.sh)
  -h, --help                         Show this help

Examples:
  $(basename "$0") --board c3 --sample philosophers --flash
  $(basename "$0") -b s3 -f
  $(basename "$0")
EOF
}

BOARD=all
SAMPLE_NAME=hello
DO_FLASH=no

while [ $# -gt 0 ]; do
  case "$1" in
    -b|--board)
      [ $# -ge 2 ] || { echo "missing value for $1" >&2; exit 2; }
      BOARD="$2"
      shift
      ;;
    -s|--sample)
      [ $# -ge 2 ] || { echo "missing value for $1" >&2; exit 2; }
      SAMPLE_NAME="$2"
      shift
      ;;
    -f|--flash)
      DO_FLASH=yes
      ;;
    -l|--log)
      # The snippet lives in the ZMK app; these are plain Zephyr samples.
      echo "ERROR: --log (zmk-usb-logging) is a ZMK app snippet — not available for samples. Use build_zmk_c3.sh / build_zmk_s3.sh --log." >&2
      exit 2
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
# Zephyr 4.4 removed the legacy "espressif" toolchain variant, so the
# Espressif cross compilers are used through the generic cross-compile
# variant instead. Per-board CROSS_COMPILE prefixes:
#   C3 (RISC-V)  -> riscv32-esp-elf-
#   S3 (Xtensa)  -> xtensa-esp32s3-elf-
export ZEPHYR_TOOLCHAIN_VARIANT=cross-compile
export ESPRESSIF_TOOLCHAIN_PATH="${ESPRESSIF_TOOLCHAIN_PATH:-/opt/espressif/tools}"
# ~/.local/bin for a pip --user esptool (required by the ESP32 SOC cmake)
export PATH="$HOME/.local/bin:$PATH"

if [ ! -d "$ESPRESSIF_TOOLCHAIN_PATH" ]; then
  echo "ERROR: ESP32 toolchain not found at $ESPRESSIF_TOOLCHAIN_PATH" >&2
  echo "Install it first — see esp-port/toolchain-esp32.md" >&2
  exit 1
fi

case "$BOARD" in
  c3|s3|all) ;;
  *)
    echo "invalid board: $BOARD (use: c3 | s3 | all)" >&2
    exit 2
    ;;
esac

case "$SAMPLE_NAME" in
  hello)        SAMPLE=zephyr/samples/hello_world ;;
  philosophers) SAMPLE=zephyr/samples/philosophers ;;
  *)
    echo "unknown sample: $SAMPLE_NAME (use: hello | philosophers)" >&2
    exit 2
    ;;
esac

# Build dirs are per-sample so hello and philosophers images don't overwrite
# each other: build/c3-hello, build/c3-philosophers, build/s3-hello, ...
C3_BOARD=esp32c3_devkitc/esp32c3
C3_DIR="build/c3-$SAMPLE_NAME"
S3_BOARD=esp32s3_devkitc/esp32s3/procpu
S3_DIR="build/s3-$SAMPLE_NAME"
USB_CONSOLE_OVERLAY="$ROOT/esp-port/fixtures/usb-console.overlay"

build() { # $1=board  $2=build dir  $3=cross-compile prefix  $4=toolchain bin dir
  # -p always = full clean rebuild, so a stale CMake cache can never pin the
  # wrong toolchain variant. Change to "-p auto" for incremental rebuilds once
  # the build dir is known-good.
  echo "=== building $1 ($SAMPLE_NAME) -> $2 (pristine) ==="
  # CROSS_COMPILE via environment (the CMake cache var is typed PATH and would
  # mangle the prefix); TOOLCHAIN_HOME has no env fallback, so pass it as -D.
  CROSS_COMPILE="$3" west build -b "$1" -d "$2" -p always "$SAMPLE" \
    -- -DEXTRA_DTC_OVERLAY_FILE="$USB_CONSOLE_OVERLAY" -DTOOLCHAIN_HOME="$4"
}

flash() { # $1=build dir
  echo "=== flashing $1 ==="
  west flash -d "$1"
}

build_c3() {
  build "$C3_BOARD" "$C3_DIR" "riscv32-esp-elf-" "$ESPRESSIF_TOOLCHAIN_PATH/riscv32-esp-elf/bin"
  if [ "$DO_FLASH" = yes ]; then flash "$C3_DIR"; fi
}

build_s3() {
  build "$S3_BOARD" "$S3_DIR" "xtensa-esp32s3-elf-" "$ESPRESSIF_TOOLCHAIN_PATH/xtensa-esp32s3-elf/bin"
  if [ "$DO_FLASH" = yes ]; then flash "$S3_DIR"; fi
}

case "$BOARD" in
  c3)  build_c3 ;;
  s3)  build_s3 ;;
  all) build_c3; build_s3 ;;
esac

echo
echo "Done. Next steps:"
if [ "$BOARD" = c3 ] || [ "$BOARD" = all ]; then
  echo "  monitor:  west monitor -d $C3_DIR"
  echo "  reflash:  west flash   -d $C3_DIR"
fi
if [ "$BOARD" = s3 ] || [ "$BOARD" = all ]; then
  echo "  monitor:  west monitor -d $S3_DIR"
  echo "  reflash:  west flash   -d $S3_DIR"
fi

echo
echo "Note on host machine:"
echo "  Goto: /Users/ryan/Documents/CobanStationeryAssets/Firmware/src/zmk"
echo "  ESP32 C3:"
echo "    esptool --port /dev/tty.usbmodem834401 write-flash 0x0 $C3_DIR/zephyr/zephyr.bin"
echo "  ESP32 S3:"
echo "    esptool --port /dev/tty.usbmodem834401 write-flash 0x0 $S3_DIR/zephyr/zephyr.bin"
