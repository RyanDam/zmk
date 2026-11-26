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
- Per-macro display names (pad10a parity): `zmk_dynamic_macro_get/set_name()`, persisted as `dynamic_macros/dn/<i>` and staged like steps; change listeners (`zmk_dynamic_macro_add_listener`) let UIs relabel live.
- Studio RPC macro subsystem for editing macros from ZMK Studio (`app/src/studio/macro_subsystem.c`), incl. `macros.set_macro_name` + `MacroInfo.name`.

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
- New `zmk_keymap_changed` event (`app/src/events/keymap_changed.c`): raised whenever the live keymap table changes underneath a UI — runtime binding edits (Studio RPC / `zmk_keymap_set_layer_binding_at_idx`), settings commit/reload, discard, layer add/remove/rename/reorder — so display pages can follow without caching bindings.
- `vkey` dev shell (`app/src/vkey_shell.c`, `CONFIG_ZMK_VKEY_SHELL`): bench rebind/clear of keymap positions and dynamic-macro renames over console (`vkey show|bind|clear|save|discard|macro-name|macro-save`), speaking the same keymap APIs Studio uses.
- Sensor init/binding load fixes.

## Studio RPC

- New RPC subsystems: `sensor`, `touchpad`, `macro`, `settings` (`app/src/studio/*_subsystem.c`); keymap/behavior subsystems extended for sensors and dynamic macros.
- The `settings` subsystem (`app/src/studio/settings_subsystem.c`, `CONFIG_ZMK_STUDIO_SETTINGS`) reports and updates Bluetooth profiles, sleep/idle configuration, preferred endpoint transport, and read-only diagnostics (battery, USB power, WPM, HID indicators, split status, uptime, activity state). Runtime activity/sleep configuration is persisted via the new `zmk_activity_*` API.
- `app/west.yml` now pulls `zmk-studio-messages` from the `cobanfirmware` (RyanDam) remote, which adds `macros.proto`, `sensors.proto`, `touchpad.proto`, `settings.proto` (with `settings.options`).
- Tab 1a subsystems (backlog a14, `CONFIG_ZMK_STUDIO_{SCREEN,THEME,STATS,POMO,ASSETS}` default-on with their features): `screen` (list/navigate + host `set_time` → the a09 time seam), `theme` (live 5-slot RGB888 get/set → a07), `stats` (config + host data push → the a11 feed seam), `pomo` (config/control/status + `zmk_pomo_state_changed` events → the a10 engine), `assets` (manifest list + `activate_asset` + a15 upload/delete below). New protos in the fork: `screen/theme/stats/pomo/assets.proto` (subsystem tags 10–14); `core.proto` gained `reboot` (pad10a parity, deferred 500 ms so the reply lands).
- a15 asset bulk upload (`CONFIG_ZMK_STUDIO_ASSET_UPLOAD`, tab1a only): the `assets` subsystem gained `upload_begin/data/end/abort/status/gc` (lockstep 1 KiB chunks, `bytes max_size:1024`; `Notification.upload_progress` at studio tag 14) backed by `app/src/studio/asset_upload.c` — staging ring drained by a dedicated preempt-low workqueue (the RPC thread never touches flash beyond acking), per-blob + manifest CRC-32 (zlib) with a **commit-last** v2 image (`ZMKAS002`: blobs first, manifest sector rewritten last with a generation counter, so an interrupted upload provably cannot corrupt), append-only gap allocation with auto-GC (quiesce handshake parks the two anim players on the 200 ms tick before any table rebuild or blob relocation), and a real `delete_asset` (manifest rewrite; built-ins survive as fallback). The store (`app/src/assets.c`) grew `zmk_assets_epoch()` guards for `'A:'` handles; `pack_assets.py` writes v2 and `build.sh` repacks `firmware/assets.bin` with it; partition carve fixed (slot1/lpcore overlaps removed — assets is now 2016 KiB @ 0xda8000). Design + rationale: `docs/a15-bulk-asset-upload-plan.md`; bench: runbook §a15.
- Transport: `CONFIG_ZMK_STUDIO_RPC_TRANSPORT_UART_PREFERRED` selects the UART transport regardless of the selected ZMK endpoint (upstream keys it off the endpoint's transport, which BLE-only display devices never reach); transports now carry a name (`"uart"`/`"gatt"`) for that selection. Bench path = UART0 (GPIO43/44, P1 header, 115200) chosen via `zmk,studio-rpc-uart` in zmk-config; scripted client + acceptance walk: `app/scripts/studio-harness/`, steps in `esp-port/tab1a-bench-runbook.md`.
- b06: Tab 1a OTG variant — `./build.sh tab1a-usb` (fragments `cobantab1a-usb.{conf,overlay}` in zmk-config) runs ZMK's USB **HID keyboard + Studio-over-CDC-ACM composite** on the USB-C port: `&usb_otg` labeled `zephyr_udc0` (DWC2 UDC) with a `zephyr,cdc-acm-uart` shim as the `zmk,studio-rpc-uart` device, console/shell moved to UART0, PREFERRED off (endpoint-keyed selection applies with a real USB endpoint). **No eFuse** — pads 19/20 are handed to OTG by the driver's `usb_wrap` init (`OTG_CONF.pad_enable`) and revert on any reset, so Serial/JTAG flashing/recovery survives; walk + port map: `esp-port/tab1a-bench-runbook.md` §b06.
- New events feeding the notifications: `zmk_display_screen_changed` (screen mgr nav), `zmk_display_theme_changed` (theme apply choke point), `zmk_stats_config_changed` (config seam now in `zmk/stats.h`, persisted `display/stats/config` moved out of the screen), `zmk_pomo_state_changed` (engine transitions, machine-readable twin of the notify seam).

## Endpoint / transport changes

- USB is always selected when connected, regardless of the preferred transport (BLE fallback only when USB is absent) — `app/src/endpoints.c`.
- BLE condition fixes and reduced logging for battery life.

## Build tooling

- `app/build_coban.sh`: one-shot build for the Coban keyboards (see AGENTS.md).
