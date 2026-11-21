# Copyright (c) 2020 The ZMK Contributors
# SPDX-License-Identifier: MIT

target_sources_ifdef(CONFIG_ZMK_DISPLAY app PRIVATE main.c)
target_sources_ifdef(CONFIG_ZMK_DISPLAY_STATUS_SCREEN_BUILT_IN app PRIVATE status_screen.c)
target_sources_ifdef(CONFIG_ZMK_DISPLAY_CLOCK_SCREEN app PRIVATE clock_screen.c)
target_sources_ifdef(CONFIG_ZMK_DISPLAY_DESIGN_SYSTEM app PRIVATE
    theme.c
    styles.c
    fonts/lv_font_tab1a_baijamjuree_72.c
    fonts/lv_font_tab1a_baijamjuree_48.c
    fonts/lv_font_tab1a_dmserif_48.c
    fonts/lv_font_tab1a_inter_28.c
    fonts/lv_font_tab1a_inter_20.c
    fonts/lv_font_tab1a_inter_16.c
    fonts/lv_font_tab1a_barlow_20.c
)

add_subdirectory_ifdef(CONFIG_ZMK_DISPLAY widgets/)
