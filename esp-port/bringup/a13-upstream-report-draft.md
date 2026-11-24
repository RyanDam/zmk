# Draft upstream report — ESP32-S3 BLE: controller wedge on first notification burst (host-only ZMK build)

> Fill `[DUMP]` placeholders from esp-port/bringup/logs/dumpall.log analysis, verify tone/format
> against zephyrproject-rtos/zephyr issue templates, then file vs `subsys: bluetooth`
> + `board: esp32` areas. CC sylvioalves (hal_espressif maintainer).

## Title
ESP32-S3 (host-only, Zephyr 4.5 + hal_espressif): any BLE notification traffic with a live link wedges the whole chip — silent, JTAG-deaf, power-cycle only

## Environment
- Board: custom ESP32-S3-WROOM-1 N16R8 prototype (bench USB power; chip rev v0.2)
- Host: Zephyr 4.5.0 (ZMK main), `CONFIG_BT_HCI_ESP32` driver, BTDM controller blobs from
  `hal_espressif` pin `cc81db0023` (2026-10-19; zero `components/bt` commits upstream since)
- Central: macOS (Sequoia) — links at interval 12 / latency 22 / timeout 2000 ms
- Same ZMK tree on ESP32-C3 (`esp32c3_supermini`): **cannot reproduce** (3+ min hammer, fine)

## Symptom
Connected peripheral, first HID notification burst (~5–30 notifications, 2 Hz) =>
- device stops responding on every interface simultaneously: no BLE traffic (Mac:
  disconnect/supervision-timeout), console silent, RTS/EN resets revive it;
- after some deaths the USB-Serial/JTAG peripheral is unresponsive to enumeration
  until USB power is cycled;
- never any panic/abort printed (dies apparently inside the blob/ISR context;
  one buffered capture ends mid `zmk_endpoint_send_report`);
- no link => immortal (same hammer with no link runs indefinitely).

## Repro (self-contained)
1. `CONFIG_ZMK_VKEY_BENCH` build (attached): SHELL/LOG/console fully compiled out;
   firmware raises real keymap position edges (press+release, HID notification) every
   500 ms from boot+12 s.
2. Pair to a Mac, reboot device, let the Mac auto-reconnect.
3. Typing starts by itself; it stops within ~10 s. Mac shows the device gone/disconnected.

## What has been ruled out (all on the same build lineage; confs listed at
https://github.com/RyanDam/zmk tree `esp-port/bringup/`, happy to extract a minimal app)
- Zephyr host + `hci_esp32` driver sequencing: **C3 control identical firmware survives**.
- ZMK layer (Studio/display/LVGL/PSRAM/vkey path): all OFF variants die identically.
- Link parameters: forcing latency 0 / different intervals does not help; idle-with-link
  survives minutes, notifications kill it.
- Peripheral-initiated HCI commands at connect: auto conn-param update OFF;
  resolving-list commands emulated in the driver (never forwarded to blob) — still dies.
- DLE/PHY-update host procedures disabled — still dies.
- VHCI force-send ("VHCI not available, sending anyway") patched to wait — still dies.
- BLE-5.0 init flag forced via `-DCONFIG_BT_LE_50_FEATURES_SUPPORTED=0` on all TUs —
  still dies (blob-side effect of this knob unverified — is it honored on S3 BTDM?).
- Power: bench USB 5V, stable; brownout ruled out (USB PHY survives while firmware dies;
  no brownout reset reason; USB-JTAG serial persists at some deaths).

## Questions for Espressif/Zephyr
1. Is `ble_50_feat_supp` honored by the S3 BTDM blob, and can a Zephyr host build pin
   the blob into a safe 4.1-only mode?
2. Known errata for S3 rev v0.2 + BLE notifications + single-core (PROCPU) builds?
3. Any expected blob-side counterparty handling the host-only build is missing
   (e.g. `BT_CTRL_BLE_ACTIVITY_*`, ACL credit config)?

## [DUMP] final-words capture
- Death mode (b): dies ~2 s after connect-complete with ZERO
  notification traffic; last console words always:
  `security_changed ... level 2` then
  `le_param_updated ... interval 12 latency 22 timeout 200` — then
  total silence (JTAG console echo dead before any user interaction).
- No 2M-PHY / DLE / PHY-update events were ever observed pre-death
  (host-side procedure logging enabled) — the blob dies before/around
  the Mac's (or iPhone's) post-connect parameter update.
- Not central-specific: same wedge vs macOS and vs iOS (iPhone paired,
  "connected then disconnected" in seconds). iOS open-link (no-SMP)
  test not possible via iOS (refuses unencrypted HID).
- Bench central harness (esp32c3, no security, logs disconnect reason):
  run result pending — will pin device-side vs central-side on an
  open link.

## Attachments to prepare
- minimal build script + `vkey_bench.c` + `.config`
- death log with timestamps
- `c3-central/` harness source (open-link central, both-ends logging)
