# west build -b nice_nano --pristine -S studio-rpc-usb-uart -S zmk-usb-logging -- -DZMK_CONFIG="/workspaces/zmk-config" -DCONFIG_ZMK_STUDIO=y -DSHIELD=cobanpad12b
# west build -b nice_nano --pristine -S studio-rpc-usb-uart -S zmk-usb-logging -- -DZMK_CONFIG="/workspaces/zmk-config" -DSHIELD=cobanpad12b
west build -b nice_nano@1 --pristine -S studio-rpc-usb-uart -- -DZMK_CONFIG="/workspaces/zmk-config" -DSHIELD=cobanpad12b
cp build/zephyr/zmk.uf2 /workspaces/zmk-config/zmk.uf2 
