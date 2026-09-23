# FEATURES.MD

Branch-specific features of `feat/coban-indicator` compared to `main` (upstream zmk).
See `git diff main...HEAD` for the full change set.

## LED indicator behavior

- New `zmk,behavior-indicator` behavior with pre-defined instances `ind_bat` (battery), `ind_con` (connectivity), `ind_lyr` (layer) — `app/dts/behaviors/indicator.dtsi`, `app/src/behaviors/behavior_indicator.c`.
- Indicator engine drives LEDs, including WS2812 RGB color support (`app/src/indicator.c`, `app/dts/bindings/indicators/zmk,indicator-leds.yaml`).
- BLE profile-clear state is reflected in the connectivity indicator.

## Dynamic macro behavior

- New `zmk,behavior-dynamic-macro` behavior (`app/src/behaviors/behavior_dynamic_macro.c`): macros editable at runtime, up to `ZMK_DYNAMIC_MACROS_COUNT` (default 16) macros with `ZMK_DYNAMIC_MACROS_MAX_STEPS` (default 64) steps each.
- Step modes: tap / press / release, with per-step wait time; steps reference any behavior binding.
- Persistence via Zephyr settings: save / discard / unsaved-changes check (`zmk_dynamic_macro_*` API in `app/include/zmk/behavior_dynamic_macro.h`).
- Studio RPC macro subsystem for editing macros from ZMK Studio (`app/src/studio/macro_subsystem.c`).

## MPR121 touchpad

- New NXP MPR121 capacitive touch sensor driver with IRQ handling, AB filtering, and debounce (`app/module/drivers/sensor/mpr121/`, binding `nxp,mpr121.yaml`).
- New touchpad module (`app/src/touchpad.c`, `CONFIG_ZMK_TOUCHPAD`): per-layer mode — gesture mode or mouse simulation — plus 7 per-layer bindings (touch start, touch end, tap, gesture left/right/up/down) and adjustable sensitivity; persisted via settings.
- New events: `mpr121_touch_start/end/move/tap` and `mpr121_gesture` (`app/src/events/mpr121_*.c`).
- Gesture detection tuned for 4-direction swipes; touchpad Studio RPC subsystem (`app/src/studio/touchpad_subsystem.c`).

## Kailh Choc encoder driver

- New `kailh,enc` sensor driver with rotation triggers and debounce (`app/module/drivers/sensor/kailh-enc/`, binding `kailh,enc.yaml`).

## GPIO key driver

- New `zmk,gpio-key` driver (`app/src/gpio_key.c`, binding `zmk,gpio-key.yaml`): monitors a GPIO pin with configurable pull-up/pull-down and debounce; used for battery/Bluetooth function keys.

## Sensor & keymap enhancements

- Keymaps support multiple bindings per sensor (`ZMK_KEYMAP_SENSORS_MAX_BINDINGS`, default 2: clockwise / counter-clockwise) — `app/src/keymap.c`.
- Sensor rotate behavior reworked: bindings now read from the keymap (supports Studio-loaded bindings), mouse emulation via binding params, macro bindings for rotate sensors, and encoder layer-handling fix — `app/src/behaviors/behavior_sensor_rotate_common.c`.
- `ec11` driver: debounce added and trigger handling improved.
- Sensor init/binding load fixes.

## Studio RPC

- New RPC subsystems: `sensor`, `touchpad`, `macro` (`app/src/studio/*_subsystem.c`); keymap/behavior subsystems extended for sensors and dynamic macros.
- `app/west.yml` now pulls `zmk-studio-messages` from the `cobanfirmware` (RyanDam) remote, which adds `macros.proto`, `sensors.proto`, `touchpad.proto`.

## Endpoint / transport changes

- USB is always selected when connected, regardless of the preferred transport (BLE fallback only when USB is absent) — `app/src/endpoints.c`.
- BLE condition fixes and reduced logging for battery life.

## Build tooling

- `app/build_coban.sh`: one-shot build for the Coban keyboards (see AGENTS.md).
