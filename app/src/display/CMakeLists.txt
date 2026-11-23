# Copyright (c) 2020 The ZMK Contributors
# SPDX-License-Identifier: MIT

target_sources_ifdef(CONFIG_ZMK_DISPLAY app PRIVATE main.c)
target_sources_ifdef(CONFIG_ZMK_DISPLAY_STATUS_SCREEN_BUILT_IN app PRIVATE status_screen.c)
target_sources_ifdef(CONFIG_ZMK_DISPLAY_CLOCK_SCREEN app PRIVATE
    clock_screen.c
    time_src.c
)
# The widget modules need the design system (style registry + ui helpers);
# the legacy clock path (no design system) does not build them.
if(CONFIG_ZMK_DISPLAY_CLOCK_SCREEN AND CONFIG_ZMK_DISPLAY_DESIGN_SYSTEM)
target_sources(app PRIVATE
    clock_widget.c
    calendar_widget.c
)
endif()
target_sources_ifdef(CONFIG_ZMK_DISPLAY_SCREEN_MGR app PRIVATE
    screen_mgr.c
    boot_screen.c
    ui_helpers.c
    # boot splash art (generated: app/scripts/display-images/gen_bootscreen.py)
    images/coban_bootscreen.c
)
target_sources_ifdef(CONFIG_ZMK_DISPLAY_POMODORO_SCREEN app PRIVATE
    pomodoro_screen.c
)
target_sources_ifdef(CONFIG_ZMK_DISPLAY_STATS_SCREEN app PRIVATE
    stats_screen.c
)
target_sources_ifdef(CONFIG_ZMK_DISPLAY_KEYMAP_SCREEN app PRIVATE
    keymap_screen.c
)
target_sources_ifdef(CONFIG_ZMK_DISPLAY_DEBUG_LAYOUT app PRIVATE
    debug_layout.c
)
target_sources_ifdef(CONFIG_ZMK_DISPLAY_ANIM app PRIVATE
    anim.c
)
if(CONFIG_ZMK_DISPLAY_SCREEN_MGR AND CONFIG_ZMK_NOTIFY)
target_sources(app PRIVATE notify_visual.c)
endif()
target_sources_ifdef(CONFIG_ZMK_DISPLAY_DESIGN_SYSTEM app PRIVATE
    theme.c
    styles.c
    fonts/lv_font_tab1a_baijamjuree_144.c
    fonts/lv_font_tab1a_baijamjuree_72.c
    fonts/lv_font_tab1a_baijamjuree_48.c
    fonts/lv_font_tab1a_dmserif_48.c
    fonts/lv_font_tab1a_inter_28.c
    fonts/lv_font_tab1a_inter_20.c
    fonts/lv_font_tab1a_inter_16.c
    fonts/lv_font_tab1a_barlow_24.c
    fonts/lv_font_tab1a_barlow_20.c
    fonts/lv_font_tab1a_icons_24.c
)

add_subdirectory_ifdef(CONFIG_ZMK_DISPLAY widgets/)
