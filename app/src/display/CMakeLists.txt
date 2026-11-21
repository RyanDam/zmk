# Copyright (c) 2020 The ZMK Contributors
# SPDX-License-Identifier: MIT

target_sources_ifdef(CONFIG_ZMK_DISPLAY app PRIVATE main.c)
target_sources_ifdef(CONFIG_ZMK_DISPLAY_STATUS_SCREEN_BUILT_IN app PRIVATE status_screen.c)
target_sources_ifdef(CONFIG_ZMK_DISPLAY_CLOCK_SCREEN app PRIVATE
    clock_screen.c
    time_src.c
)
target_sources_ifdef(CONFIG_ZMK_DISPLAY_SCREEN_MGR app PRIVATE
    screen_mgr.c
    boot_screen.c
    ui_helpers.c
)
target_sources_ifdef(CONFIG_ZMK_DISPLAY_POMODORO_SCREEN app PRIVATE
    pomodoro_screen.c
)
target_sources_ifdef(CONFIG_ZMK_DISPLAY_STATS_SCREEN app PRIVATE
    stats_screen.c
)
if(CONFIG_ZMK_DISPLAY_SCREEN_MGR AND CONFIG_ZMK_NOTIFY)
target_sources(app PRIVATE notify_visual.c)
endif()
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
