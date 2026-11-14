#!/bin/bash
# Build the Coban keyboards (cobanpad12b + cobanpad16a) for nice_nano@1.
#
# Usage (works from anywhere in the repo; sh or bash):
#   bash app/build_coban.sh
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

ZMK_CONFIG="${ZMK_CONFIG:-$(cd "$ROOT/.." && pwd)/zmk-config}"
if [ ! -d "$ZMK_CONFIG" ]; then
  echo "ERROR: ZMK config repo not found at $ZMK_CONFIG" >&2
  echo "Set ZMK_CONFIG to the external config repo containing the cobanpad shields." >&2
  exit 1
fi

# Per-shield build dirs: a --pristine build in the shared build/ dir would
# wipe the ESP32 build dirs (build/c3-zmk, ...) that live alongside it.
# Build cobanpad12b
west build -b nice_nano@1 --pristine -S studio-rpc-usb-uart -s app -d build/pad12b -- -DZMK_CONFIG="$ZMK_CONFIG" -DSHIELD=cobanpad12b
cp build/pad12b/zephyr/zmk.uf2 "$ZMK_CONFIG/zmk-12b.uf2"

# Build cobanpad16a
west build -b nice_nano@1 --pristine -S studio-rpc-usb-uart -s app -d build/pad16a -- -DZMK_CONFIG="$ZMK_CONFIG" -DSHIELD=cobanpad16a
cp build/pad16a/zephyr/zmk.uf2 "$ZMK_CONFIG/zmk-16a.uf2"
