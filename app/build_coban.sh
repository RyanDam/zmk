#!/bin/bash
# Build the Coban keyboards (cobanpad12b + cobanpad16a) for nice_nano@1.
#
# Usage (works from anywhere in the repo; sh or bash):
#   bash app/build_coban.sh            # release-shaped build
#   bash app/build_coban.sh --log      # same + verbose logging (USB CDC)
#   ZMK_BUILD_LOG=1 bash app/build_coban.sh
#
# --log / -l (or env ZMK_BUILD_LOG=1): adds the zmk-usb-logging snippet —
# the pads' canonical debug path: a dedicated "ZMK Logging" USB CDC next to
# the Studio RPC one, with the console + shell redirected to it.
#
# The cobanpad shields live in the external ZMK config repo (sibling of this
# repo by default): $ZMK_CONFIG/boards/shields/. Override the location with
# the ZMK_CONFIG environment variable. In the dev container the sibling is
# /workspaces/zmk-config, so the default works there too.

# POSIX sh compatible (works under sh/dash and bash). No pipelines are used,
# so "set -o pipefail" is not needed.
set -eu

# Run from the workspace root (parent of app/) so the build/ dir resolves
# correctly, regardless of where the script is invoked.
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

usage() {
  cat <<EOF
Usage: $(basename "$0") [options]

Options:
  -l, --log   Add the zmk-usb-logging snippet (logs on a dedicated
              "ZMK Logging" USB CDC; console + shell move to it)
  -h, --help  Show this help
EOF
}

# --log flag or ZMK_BUILD_LOG=1 (set by the workspace build.sh --log).
DO_LOG="${ZMK_BUILD_LOG:-no}"
case "$DO_LOG" in 1|yes|y|true) DO_LOG=yes ;; *) DO_LOG=no ;; esac

while [ $# -gt 0 ]; do
  case "$1" in
    -l|--log)
      DO_LOG=yes
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

ZMK_CONFIG="${ZMK_CONFIG:-$(cd "$ROOT/.." && pwd)/zmk-config}"
if [ ! -d "$ZMK_CONFIG" ]; then
  echo "ERROR: ZMK config repo not found at $ZMK_CONFIG" >&2
  echo "Set ZMK_CONFIG to the external config repo containing the cobanpad shields." >&2
  exit 1
fi

SNIPPETS="-S studio-rpc-usb-uart"
if [ "$DO_LOG" = yes ]; then
  SNIPPETS="$SNIPPETS -S zmk-usb-logging"
fi

# Per-shield build dirs: a --pristine build in the shared build/ dir would
# wipe the ESP32 build dirs (build/c3-zmk, ...) that live alongside it.
# Build cobanpad12b
# shellcheck disable=SC2086
west build -b nice_nano@1 --pristine $SNIPPETS -s app -d build/pad12b -- -DZMK_CONFIG="$ZMK_CONFIG" -DSHIELD=cobanpad12b
cp build/pad12b/zephyr/zmk.uf2 "$ZMK_CONFIG/zmk-12b.uf2"

# Build cobanpad16a
# shellcheck disable=SC2086
west build -b nice_nano@1 --pristine $SNIPPETS -s app -d build/pad16a -- -DZMK_CONFIG="$ZMK_CONFIG" -DSHIELD=cobanpad16a
cp build/pad16a/zephyr/zmk.uf2 "$ZMK_CONFIG/zmk-16a.uf2"
