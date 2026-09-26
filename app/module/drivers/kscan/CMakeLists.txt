# Copyright (c) 2020-2023 The ZMK Contributors
# SPDX-License-Identifier: MIT

# Zephyr's own drivers/kscan library was removed in Zephyr 4.2, so this
# directory creates its own library instead of amending the Zephyr one.
# Temporary: removed together with the matrix-input migration (backlog a01).
zephyr_library()

zephyr_library_sources_ifdef(CONFIG_ZMK_KSCAN_GPIO_DRIVER kscan_gpio.c)
zephyr_library_sources_ifdef(CONFIG_ZMK_KSCAN_GPIO_MATRIX kscan_gpio_matrix.c)
zephyr_library_sources_ifdef(CONFIG_ZMK_KSCAN_GPIO_CHARLIEPLEX kscan_gpio_charlieplex.c)
zephyr_library_sources_ifdef(CONFIG_ZMK_KSCAN_GPIO_DIRECT kscan_gpio_direct.c)
zephyr_library_sources_ifdef(CONFIG_ZMK_KSCAN_GPIO_DEMUX kscan_gpio_demux.c)
zephyr_library_sources_ifdef(CONFIG_ZMK_KSCAN_MOCK_DRIVER kscan_mock.c)
zephyr_library_sources_ifdef(CONFIG_ZMK_KSCAN_COMPOSITE_DRIVER kscan_composite.c)
