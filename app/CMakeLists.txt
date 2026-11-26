cmake_minimum_required(VERSION 3.13.1)

set(CONFIG_APPLICATION_DEFINED_SYSCALL true)

set(ZEPHYR_BOARD_ALIASES "boards/aliases.cmake")
set(ZEPHYR_EXTRA_MODULES "${ZMK_EXTRA_MODULES};${CMAKE_CURRENT_SOURCE_DIR}/module;${CMAKE_CURRENT_SOURCE_DIR}/keymap-module")

# Zephyr 4.4 no longer adds the application source dir to SNIPPET_ROOT by
# default; register it so the snippets in app/snippets are discoverable.
set(SNIPPET_ROOT "${CMAKE_CURRENT_LIST_DIR}")

# Find Zephyr. This also loads Zephyr's build system.
find_package(Zephyr REQUIRED HINTS ../zephyr)
project(zmk)

zephyr_linker_sources(SECTIONS include/linker/zmk-behaviors.ld)
zephyr_linker_sources(SECTIONS include/linker/zmk-display-screens.ld)
zephyr_linker_sources(RODATA include/linker/zmk-events.ld)

if(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS)
  zephyr_linker_sources(DATA_SECTIONS include/linker/zmk-behavior-local-id-map.ld)
endif()

zephyr_syscall_header(${APPLICATION_SOURCE_DIR}/include/drivers/behavior.h)
zephyr_syscall_header(${APPLICATION_SOURCE_DIR}/include/drivers/input_processor.h)
zephyr_syscall_header(${APPLICATION_SOURCE_DIR}/include/drivers/ext_power.h)

# Add your source file to the "app" target. This must come after
# find_package(Zephyr) which defines the target.
target_include_directories(app PRIVATE include)
add_subdirectory(src/boot)
target_sources(app PRIVATE src/stdlib.c)
target_sources(app PRIVATE src/activity.c)
target_sources_ifdef(CONFIG_ZMK_NOTIFY app PRIVATE src/notify.c)
target_sources_ifdef(CONFIG_ZMK_POMODORO app PRIVATE src/pomodoro.c)
target_sources_ifdef(CONFIG_ZMK_STATS app PRIVATE src/stats.c)
target_sources_ifdef(CONFIG_ZMK_VKEY_SHELL app PRIVATE src/vkey_shell.c)
target_sources_ifdef(CONFIG_ZMK_VKEY_BENCH app PRIVATE src/vkey_bench.c)
target_sources_ifdef(CONFIG_ZMK_ASSETS app PRIVATE src/assets.c)
target_sources_ifdef(CONFIG_ZMK_ASSETS app PRIVATE src/events/assets_changed.c)
target_sources_ifdef(CONFIG_ZMK_DISPLAY_ANIM_BUILTIN app PRIVATE src/assets_builtin.c)
if(CONFIG_ZMK_DISPLAY_ANIM_BUILTIN)
# The built-in default GIF is embedded at build time via .incbin from
# src/display/assets/animbg.gif — committing the generated ~13 MB C
# array is not an option (see src/assets_builtin.c).
target_compile_definitions(app PRIVATE
    ZMK_ANIMBG_GIF_PATH=${CMAKE_CURRENT_SOURCE_DIR}/src/display/assets/animbg.gif
)
endif()
target_sources(app PRIVATE src/behavior.c)
target_sources_ifdef(CONFIG_ZMK_KSCAN_SIDEBAND_BEHAVIORS app PRIVATE src/kscan_sideband_behaviors.c)
target_sources(app PRIVATE src/matrix_transform.c)
target_sources(app PRIVATE src/physical_layouts.c)
target_sources(app PRIVATE src/sensors.c)
target_sources_ifdef(CONFIG_ZMK_WPM app PRIVATE src/wpm.c)
target_sources(app PRIVATE src/event_manager.c)
target_sources_ifdef(CONFIG_ZMK_PM app PRIVATE src/pm.c)
target_sources_ifdef(CONFIG_ZMK_EXT_POWER app PRIVATE src/ext_power_generic.c)
target_sources_ifdef(CONFIG_ZMK_GPIO_KEY_WAKEUP_TRIGGER app PRIVATE src/gpio_key_wakeup_trigger.c)
target_sources(app PRIVATE src/events/activity_state_changed.c)
target_sources(app PRIVATE src/events/position_state_changed.c)
target_sources(app PRIVATE src/events/keymap_changed.c)
target_sources_ifdef(CONFIG_ZMK_DISPLAY_SCREEN_MGR app PRIVATE src/events/screen_changed.c)
target_sources_ifdef(CONFIG_ZMK_DISPLAY app PRIVATE src/events/theme_changed.c)
target_sources_ifdef(CONFIG_ZMK_STATS app PRIVATE src/events/stats_config_changed.c)
target_sources_ifdef(CONFIG_ZMK_POMODORO app PRIVATE src/events/pomo_state_changed.c)
target_sources(app PRIVATE src/events/sensor_event.c)
target_sources_ifdef(CONFIG_ZMK_WPM app PRIVATE src/events/wpm_state_changed.c)
target_sources_ifdef(CONFIG_ZMK_USB_DEVICE app PRIVATE src/events/usb_conn_state_changed.c)
target_sources(app PRIVATE src/behaviors/behavior_reset.c)
target_sources_ifdef(CONFIG_ZMK_EXT_POWER app PRIVATE src/behaviors/behavior_ext_power.c)
target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_SOFT_OFF app PRIVATE src/behaviors/behavior_soft_off.c)
add_subdirectory_ifdef(CONFIG_ZMK_POINTING src/pointing/)
if ((NOT CONFIG_ZMK_SPLIT) OR CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
  target_sources(app PRIVATE src/hid.c)
  target_sources(app PRIVATE src/behaviors/behavior_key_press.c)
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_KEY_TOGGLE app PRIVATE src/behaviors/behavior_key_toggle.c)
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_HOLD_TAP app PRIVATE src/behaviors/behavior_hold_tap.c)
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_STICKY_KEY app PRIVATE src/behaviors/behavior_sticky_key.c)
  target_sources(app PRIVATE src/behaviors/behavior_caps_word.c)
  target_sources(app PRIVATE src/behaviors/behavior_key_repeat.c)
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_MACRO app PRIVATE src/behaviors/behavior_macro.c)
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO app PRIVATE src/behaviors/behavior_dynamic_macro.c)
  target_sources(app PRIVATE src/behaviors/behavior_momentary_layer.c)
  target_sources(app PRIVATE src/behaviors/behavior_mod_morph.c)
  target_sources(app PRIVATE src/behaviors/behavior_outputs.c)
  target_sources(app PRIVATE src/behaviors/behavior_toggle_layer.c)
  target_sources(app PRIVATE src/behaviors/behavior_to_layer.c)
  target_sources(app PRIVATE src/behaviors/behavior_transparent.c)
  target_sources(app PRIVATE src/behaviors/behavior_none.c)
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_SENSOR_ROTATE app PRIVATE src/behaviors/behavior_sensor_rotate.c)
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_SENSOR_ROTATE_VAR app PRIVATE src/behaviors/behavior_sensor_rotate_var.c)
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_SENSOR_ROTATE_COMMON app PRIVATE src/behaviors/behavior_sensor_rotate_common.c)
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_MOUSE_KEY_PRESS app PRIVATE src/behaviors/behavior_mouse_key_press.c)
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_STUDIO_UNLOCK app PRIVATE src/behaviors/behavior_studio_unlock.c)
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_INPUT_TWO_AXIS app PRIVATE src/behaviors/behavior_input_two_axis.c)
  target_sources(app PRIVATE src/combo.c)
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_TAP_DANCE app PRIVATE src/behaviors/behavior_tap_dance.c)
  target_sources(app PRIVATE src/behavior_queue.c)
  target_sources(app PRIVATE src/conditional_layer.c)
  target_sources(app PRIVATE src/endpoints.c)
  target_sources(app PRIVATE src/events/endpoint_changed.c)
  target_sources(app PRIVATE src/hid_listener.c)
  target_sources(app PRIVATE src/keymap.c)
  target_sources(app PRIVATE src/events/layer_state_changed.c)
  target_sources(app PRIVATE src/events/modifiers_state_changed.c)
  target_sources(app PRIVATE src/events/keycode_state_changed.c)
  target_sources_ifdef(CONFIG_ZMK_HID_INDICATORS app PRIVATE src/hid_indicators.c)

  if (CONFIG_ZMK_BLE)
    target_sources(app PRIVATE src/events/ble_active_profile_changed.c)
    target_sources(app PRIVATE src/behaviors/behavior_bt.c)
    target_sources(app PRIVATE src/ble.c)
    target_sources(app PRIVATE src/hog.c)
  endif()
endif()

target_sources_ifdef(CONFIG_ZMK_RGB_UNDERGLOW app PRIVATE src/behaviors/behavior_rgb_underglow.c)
target_sources_ifdef(CONFIG_ZMK_BACKLIGHT app PRIVATE src/behaviors/behavior_backlight.c)

target_sources_ifdef(CONFIG_ZMK_BATTERY_REPORTING app PRIVATE src/events/battery_state_changed.c)
target_sources_ifdef(CONFIG_ZMK_BATTERY_REPORTING app PRIVATE src/battery.c)

target_sources_ifdef(CONFIG_ZMK_HID_INDICATORS app PRIVATE src/events/hid_indicators_changed.c)
add_subdirectory_ifdef(CONFIG_ZMK_HID_INDICATORS src/indicators)

target_sources_ifdef(CONFIG_ZMK_SPLIT app PRIVATE src/events/split_peripheral_status_changed.c)
add_subdirectory_ifdef(CONFIG_ZMK_SPLIT src/split)

target_sources_ifdef(CONFIG_MPR121 app PRIVATE src/events/mpr121_gesture_event.c)
target_sources_ifdef(CONFIG_MPR121 app PRIVATE src/events/mpr121_touch_start_event.c)
target_sources_ifdef(CONFIG_MPR121 app PRIVATE src/events/mpr121_touch_move_event.c)
target_sources_ifdef(CONFIG_MPR121 app PRIVATE src/events/mpr121_touch_end_event.c)
target_sources_ifdef(CONFIG_MPR121 app PRIVATE src/events/mpr121_touch_tap_event.c)
target_sources_ifdef(CONFIG_ZMK_TOUCHPAD app PRIVATE src/touchpad.c)

target_sources_ifdef(CONFIG_ZMK_USB_DEVICE app PRIVATE src/usb.c)
target_sources_ifdef(CONFIG_ZMK_USB app PRIVATE src/usb_hid.c)
target_sources_ifdef(CONFIG_ZMK_RGB_UNDERGLOW app PRIVATE src/rgb_underglow.c)
target_sources_ifdef(CONFIG_ZMK_BACKLIGHT app PRIVATE src/backlight.c)
target_sources_ifdef(CONFIG_ZMK_LOW_PRIORITY_WORK_QUEUE app PRIVATE src/workqueue.c)
target_sources(app PRIVATE src/indicator.c)
target_sources(app PRIVATE src/behaviors/behavior_indicator.c)
target_sources(app PRIVATE src/gpio_key.c)
target_sources(app PRIVATE src/main.c)

add_subdirectory(src/display/)
add_subdirectory(src/tab1a/)
add_subdirectory_ifdef(CONFIG_SETTINGS src/settings/)

if (CONFIG_ZMK_STUDIO_RPC)
  # For some reason this is failing if run from a different sub-file.
  list(APPEND CMAKE_MODULE_PATH ${ZEPHYR_BASE}/modules/nanopb)

  include(nanopb)

    # Turn off the default nanopb behavior
  set(NANOPB_GENERATE_CPP_STANDALONE OFF)

  nanopb_generate_cpp(proto_srcs proto_hdrs RELPATH ${ZEPHYR_ZMK_STUDIO_MESSAGES_MODULE_DIR}
                        ${ZEPHYR_ZMK_STUDIO_MESSAGES_MODULE_DIR}/proto/zmk/studio.proto
                        ${ZEPHYR_ZMK_STUDIO_MESSAGES_MODULE_DIR}/proto/zmk/meta.proto
                        ${ZEPHYR_ZMK_STUDIO_MESSAGES_MODULE_DIR}/proto/zmk/core.proto
                        ${ZEPHYR_ZMK_STUDIO_MESSAGES_MODULE_DIR}/proto/zmk/behaviors.proto
                        ${ZEPHYR_ZMK_STUDIO_MESSAGES_MODULE_DIR}/proto/zmk/keymap.proto
                        ${ZEPHYR_ZMK_STUDIO_MESSAGES_MODULE_DIR}/proto/zmk/macros.proto
                        ${ZEPHYR_ZMK_STUDIO_MESSAGES_MODULE_DIR}/proto/zmk/sensors.proto
                        ${ZEPHYR_ZMK_STUDIO_MESSAGES_MODULE_DIR}/proto/zmk/touchpad.proto
                        ${ZEPHYR_ZMK_STUDIO_MESSAGES_MODULE_DIR}/proto/zmk/settings.proto
                        ${ZEPHYR_ZMK_STUDIO_MESSAGES_MODULE_DIR}/proto/zmk/screen.proto
                        ${ZEPHYR_ZMK_STUDIO_MESSAGES_MODULE_DIR}/proto/zmk/theme.proto
                        ${ZEPHYR_ZMK_STUDIO_MESSAGES_MODULE_DIR}/proto/zmk/stats.proto
                        ${ZEPHYR_ZMK_STUDIO_MESSAGES_MODULE_DIR}/proto/zmk/pomo.proto
                        ${ZEPHYR_ZMK_STUDIO_MESSAGES_MODULE_DIR}/proto/zmk/assets.proto
  )

  target_include_directories(app PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
  target_sources(app PRIVATE ${proto_srcs} ${proto_hdrs})

  add_subdirectory(src/studio)
endif()

zephyr_cc_option(-Wfatal-errors)
