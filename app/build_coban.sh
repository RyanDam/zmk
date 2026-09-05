#!/bin/bash
# # Build cobanpad12b
# west build -b nice_nano@1 --pristine -S studio-rpc-usb-uart -S zmk-usb-logging -s app -- -DZMK_CONFIG="/workspaces/zmk-config" -DSHIELD=cobanpad12b
# cp build/zephyr/zmk.uf2 /workspaces/zmk-config/zmk-12b.uf2

# Build cobanpad16a
west build -b nice_nano@1 --pristine -S studio-rpc-usb-uart -S zmk-usb-logging -s app -- -DZMK_CONFIG="/workspaces/zmk-config" -DSHIELD=cobanpad16a
cp build/zephyr/zmk.uf2 /workspaces/zmk-config/zmk-16a.uf2
