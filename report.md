# ESP32-C3 / ESP32-S3 support investigation

**Scope:** investigate adding ZMK support for Espressif ESP32-C3 and ESP32-S3; do not implement it.  
**Investigated:** 2026-09-23.

## Executive recommendation

Implement **BLE-first board variants** for the C3 and S3, but take the opportunity to upgrade ZMK to **Zephyr 4.4.1** first. The required Zephyr SoC, GPIO, flash, settings prerequisites, and Espressif in-process Bluetooth HCI driver already exist in 4.1; 4.4 adds the missing S3 USB-OTG platform support and 4.4.1 adds relevant Espressif Bluetooth/PSRAM fixes. This is a repository-wide migration because ZMK must first replace its legacy kscan and USB stacks. If a fast proof of concept is needed before that work completes, use the existing 4.1 fork for BLE only. Begin with the upstream development boards as bring-up targets:

| Target | ZMK target qualifier | Recommended initial capability |
| --- | --- | --- |
| ESP32-C3 SuperMini (common ESP32-C3FX4/FN4, 4 MB) | `esp32c3_supermini/esp32c3` | BLE HID only |
| ESP32-S3 SuperMini (verify actual chip; common S3FH4R2, 4 MB flash + 2 MB QSPI PSRAM) | new `esp32s3_supermini/esp32s3/procpu` board, then `zmk` variant | BLE HID only; wired USB HID is conditional and must pass the USB-PHY gate below |

Do **not** make USB a requirement for the first C3 target: the C3 has a fixed-function USB Serial/JTAG peripheral, not USB OTG/device HID hardware. The S3 does have USB OTG and Zephyr 4.4 supplies its DWC2 node, but the common SuperMini routes its sole USB-C connector to GPIO19/20, whose internal PHY defaults to USB Serial/JTAG. Espressif states that using this internal PHY for OTG requires the permanent `USB_PHY_SEL` eFuse; otherwise OTG needs an external PHY. Therefore S3 USB HID is a **conditional, potentially irreversible hardware configuration**, not an assumed feature of the SuperMini support. The first S3 release must be BLE-only.

The primary product risk is **battery life**, not basic functionality. Espressif labels C3 production-stable from Zephyr 4.0, but its status matrix lists C3/S3 low power as work in progress. An earlier ZMK S3 proof of concept reported roughly 50--100 mA and was abandoned because of power and USB gaps. Treat a battery-powered wireless keyboard as experimental until measured over the full idle/advertising/connected/sleep lifecycle.

## Current repository findings

ZMK already separates application logic from board/SoC wiring, so a new Espressif port should be mostly board configuration rather than a new application architecture.

* `app/west.yml` pins Zephyr to `v4.1.0+zmk-fixes`.
* `app/Kconfig` enables ZMK BLE through the standard Zephyr host stack (`BT`, SMP, peripheral role, settings) and enables USB separately through Zephyr's now-deprecated legacy USB device/HID stack.
* `app/src/ble.c` is SoC-neutral: it uses Zephyr's Bluetooth host APIs for advertising, bonds, profiles, security, and split transport. It does not require a Nordic controller.
* Existing board variants use `board.yml` plus a `<board>_<soc>_zmk_defconfig` and `<board>_<soc>_zmk.dts`. This is the appropriate pattern for Espressif variants.
* Persistent ZMK bonds/profiles require a writable flash partition and a working Zephyr settings backend. Existing boards generally use NVS/`SETTINGS_NVS`; an earlier S3 proof of concept used FCB because NVS then crashed. NVS must be validated again on the selected current Espressif HAL before committing to it.

## Why legacy kscan cannot be carried to Zephyr 4.4

This is not merely a preference for a newer abstraction: the upstream kscan subsystem is **absent** from the intended Zephyr baseline. Zephyr deprecated the API in 4.1 after moving its in-tree keyboard drivers to the Input subsystem, with an announced 4.2 removal; the removal commit (`60a9a202`) deletes the header, Kconfig subsystem, bindings, drivers, samples, and all references. Consequently a ZMK build on 4.4 fails immediately on `#include <zephyr/drivers/kscan.h>`, `CONFIG_KSCAN`, and `struct kscan_driver_api`.

The architectural difference matters:

| Legacy kscan | Current Input / keyboard-matrix model |
| --- | --- |
| Application registers one callback on a scan device through `kscan_config()`, then explicitly enables/disables that callback. | Drivers publish standardized `input_event` records; consumers register `INPUT_CALLBACK_DEFINE()` listeners, optionally filtered to one input device. |
| Event payload is bespoke: `(device, row, column, pressed)`. | Matrix position is a synchronized event group: `INPUT_ABS_X` (column), `INPUT_ABS_Y` (row), then `INPUT_BTN_TOUCH` (state, `sync=true`). This allows the shared Input pipeline to carry keyboards, buttons, pointers, touch, and other input types. |
| Lifecycle is tied to callback enablement, which ZMK uses when switching physical layouts. | Driver lifecycle is device power management; listeners are static. ZMK must filter events by active layout and preserve suspend/wakeup behavior without `kscan_enable_callback()`/`kscan_disable_callback()`. |
| Each out-of-tree driver owns the legacy API contract. | Generic matrices can use upstream `gpio-kbd-matrix`; special scanners implement/publish Input events while retaining their hardware-specific scan/debounce logic. |

ZMK has a useful head start, but it is incomplete. `app/src/physical_layouts.c` already accepts `zmk,matrix-input`, installs Input callbacks, and converts the X/Y/touch triplet back into ZMK position events. It still includes `kscan.h`, contains the legacy callback/lifecycle path, and `struct zmk_physical_layout` retains `kscan`. Its seven legacy driver translation units (`gpio` support plus matrix, direct, demux, charlieplex, mock, and composite) are guarded by `if KSCAN` and expose `kscan_driver_api`; all must migrate or be removed.

**Migration implication:** this is an API and behavior migration, not a mechanical include rename. Preserve ZMK-specific debounce timing, diode direction, interrupt/poll wake strategy, ghost handling, physical-layout switching, sideband behaviors, composite routing, split semantics, and test injection. Upstream `gpio-kbd-matrix` can likely replace the ordinary matrix case after equivalence testing, but it does not automatically cover ZMK's direct, demux, charlieplex, composite, or sideband features.

## Why the legacy USB stack is similar—but not identical

The USB work has the same strategic conclusion (migrate before treating 4.4 as a durable baseline), but a different immediate status: Zephyr 4.4.1 still contains the legacy stack. It marks `CONFIG_USB_DEVICE_STACK` as `DEPRECATED`, and marks the descriptor macros, `usb_enable()`, `usb_wakeup_request()`, `usb_hid_register_device()`, `hid_int_ep_write()`, and related APIs deprecated in favor of the UDC/USBD stack. A short-lived 4.4 build may therefore compile with warnings; unlike kscan, it is not yet an immediate missing-header failure.

The replacement is also a real lifecycle/model change rather than a renamed function:

| Legacy USB device stack used by ZMK | Current USB device stack |
| --- | --- |
| Global, singleton-style enablement through `usb_enable(status_cb)` and `usb_dc_status_code`. | An explicit `usbd_context`, normally created with `USBD_DEVICE_DEFINE()` and bound to a `zephyr_udc0` controller. |
| Linker-section descriptor macros and class registration; legacy HID callbacks and `hid_int_ep_write()`. | Descriptors, speed-specific configurations, and HID function instances are attached to the context before `usbd_init()` / `usbd_enable()`; HID uses the `usbd_hid_device` API. |
| Status callback drives ZMK's “powered/HID-ready” state. | `usbd_msg_register_cb()` supplies context-specific lifecycle, VBUS, and configuration messages. |
| Old USB-device-controller API. | UDC driver API, designed only for the current stack, supports multiple controllers and both full/high-speed device operation. |

That distinction is especially relevant to ESP32-S3: Zephyr's S3 DWC2 enablement is a UDC (`zephyr_udc0`) path. Retaining ZMK's old stack would make the new S3 USB target dependent on deprecated code and potentially controller compatibility that is no longer the focus of upstream testing. ZMK's affected core is at least `app/src/usb.c`, `app/src/usb_hid.c`, `app/Kconfig`, USB-guarded activity/display code, USB board defaults, and Studio UART/CDC integration where enabled. Preserve keyboard, consumer, mouse, boot-protocol, LED output, feature reports, remote wakeup, endpoint serialization, and BLE/USB transport switching.

**Bottom line:** kscan is a hard 4.2+ build blocker; legacy USB is a still-buildable but deprecated compatibility risk. Both belong before permanent ESP support because the selected S3 wired-HID path needs the current USB stack, while all ZMK boards need the post-kscan input architecture.

## First-target board research: SuperMini family

"SuperMini" is a third-party form factor, not an Espressif-controlled board specification. Flash/PSRAM capacity, LED wiring, battery circuit, exposed pads, and sometimes the USB implementation vary by seller. The board definition must support an explicitly qualified reference sample, not every board bearing this name.

### ESP32-C3 SuperMini

Zephyr 4.4 already includes the `esp32c3_supermini/esp32c3` board. It describes a 4-MB C3 board, maps the BOOT button to GPIO9, the active-low blue LED to GPIO8, enables USB Serial/JTAG for console/flashing, and enables the Espressif Bluetooth HCI node. This is the correct upstream base for a ZMK `zmk` board extension; no new generic C3 board definition is needed.

* The common board exposes GPIO0--10 and GPIO20/21. Reserve GPIO18/19 for the USB Serial/JTAG D-/D+ signals, GPIO9 for BOOT/download, GPIO8 for its onboard active-low LED, and avoid loading strapping pins GPIO2, GPIO8, and GPIO9 at reset.
* Prefer the direct keyboard matrix fixture on GPIO0, GPIO1, GPIO3, GPIO4, GPIO5, GPIO10, GPIO20, and GPIO21, subject to verified carrier wiring. GPIO20/21 are UART0 defaults, so disabling UART console is appropriate if either is used in a matrix.
* Do not use GPIO18/19 as matrix pins: doing so loses the USB-C flash/console recovery path. The USB Serial/JTAG controller is fixed-function and cannot become a HID keyboard.
* The upstream board is labelled "not actively maintained" and explicitly warns of vendor variation. ZMK must maintain its own hardware qualification against the selected physical board revision.

### ESP32-S3 SuperMini

No upstream Zephyr 4.4 `esp32s3_supermini` board exists. Add a dedicated custom board (not a DevKitC extension) because its memory topology, exposed pins, LED, and USB wiring differ. The common contemporary sample is marked `ESP32-S3FH4R2`: 4 MB embedded flash and 2 MB quad-SPI PSRAM. Its Zephyr starting include is `espressif/esp32s3/esp32s3_wroom_n4r2.dtsi`, which configures the same 4-MB/2-MB memory sizes, but the actual chip marking must be verified before selecting it.

* Preserve GPIO19 (USB D-) and GPIO20 (USB D+) for the USB-C connector. Do not use them for a keyboard matrix.
* Reserve GPIO0 for BOOT/recovery. Avoid external reset-time loading of S3 strapping pins GPIO3, GPIO45, and GPIO46. Avoid GPIO45 as a general matrix pin because it participates in VDD_SPI selection.
* The common board's WS2812 LED is on GPIO48; make it an optional board LED and do not use it in the default matrix. Treat this mapping as revision-specific.
* Preserve GPIO43/44 as UART0 recovery/logging pins until USB-HID/fuse behavior is proven; they are the fallback debug path if the USB-C configuration becomes unavailable.
* Use a conservative first matrix pin set GPIO1, GPIO2, GPIO4--GPIO8, GPIO15--GPIO18, and GPIO21. GPIO33--38 may be usable on the common 2-MB quad-PSRAM part but must remain excluded from the portable/default mapping because memory variants can reserve them.
* Keep PSRAM disabled for the first BLE keyboard unless memory measurement demonstrates a need. It is not necessary for basic ZMK, and an incorrect PSRAM topology/mode turns into boot failures.

### S3 USB-PHY gate

Before implementing an S3 USB-HID variant, inspect the exact SuperMini schematic/PCB and execute this decision tree:

1. If the board provides a separately routed external USB PHY for OTG, test that path without fuses.
2. If the USB-C connector only reaches GPIO19/20 and the internal PHY is still assigned to USB Serial/JTAG, do **not** burn eFuses during normal development. First prove BLE firmware, boot recovery through UART, and a spare sacrificial board procedure.
3. Only if documented, repeatable tests show that setting `USB_PHY_SEL` permits Zephyr DWC2 HID on that specific revision—and the project accepts permanent loss/change of normal USB Serial/JTAG behavior—may an explicitly named `usb_otg_fused` S3 variant be considered.
4. Otherwise, support S3 SuperMini as BLE-only and use USB-C solely for power/flashing/logging.

This gate supersedes the earlier assumption that the presence of an S3 OTG controller automatically yields safe USB HID on the SuperMini connector.

## Zephyr capability assessment

### What Zephyr 4.1 already provides

Both target **SoC families** have upstream Zephyr 4.1 support, including GPIO, I2C, SPI, flash, partitions, TRNG, UART, LEDC PWM, ADC, and Bluetooth HCI nodes. The C3 SuperMini board definition arrives upstream later; the S3 SuperMini needs a custom definition.

* **C3:** RISC-V, single core, 400 KB SRAM, BLE 5 controller, 22 programmable GPIOs. Zephyr 4.4 includes the actual SuperMini target `esp32c3_supermini/esp32c3`.
* **S3:** Xtensa LX7, dual core, 512 KB SRAM, BLE 5 controller, 45 programmable GPIOs, and native USB OTG hardware. The new SuperMini board must use the **PROCPU** target (`esp32s3_supermini/esp32s3/procpu`), not introduce AMP/SMP into the initial port.
* The Espressif HCI driver (`CONFIG_BT_ESP32`) is selected from the enabled `espressif,esp32-bt-hci` devicetree node, links the proprietary RF blobs, and exposes the controller to Zephyr's ordinary host stack. That is compatible with ZMK's existing BLE implementation.
* Both upstream boards select `zephyr,bt-hci = &esp32_bt_hci` and enable that node. A ZMK board variant that extends or includes these upstream boards should retain this relationship.
* Espressif RF blobs are mandatory for actual BLE. Developer/CI setup needs `west blobs fetch hal_espressif` after `west update`; source-only builds can use `BUILD_ONLY_NO_BLOBS`, but cannot run on hardware.

### What 4.1 does not provide for this project

* **ESP32-S3 USB OTG:** the 4.1 S3 SoC DTS has only USB Serial/JTAG; no `usb_otg`/DWC2 controller node. Consequently ZMK's `CONFIG_ZMK_USB=y` cannot be a supported S3 configuration on the present baseline.
* **ESP32-C3 USB HID:** C3 lacks an OTG peripheral, so its USB Serial/JTAG interface is for flashing/console, not a ZMK USB keyboard interface.
* No upstream ZMK ESP32 board variants, CI builds, hardware-in-loop BLE validation, power budget, or documented flash/settings configuration exist today.

### Newer Zephyr requirement

Zephyr **4.4** contains the S3 `usb_otg` node (`espressif,esp32-usb-otg`, `snps,dwc2`) and the DevKitC demonstrates its enablement as `zephyr_udc0`. Espressif's latest support matrix also marks S3 USB OTG supported. This establishes a credible driver path, but the SuperMini's internal-PHY routing/eFuse requirement means it is not proof that wired HID is safe or available on that board.

**Version decision:**

1. Prefer a repository-wide upgrade to Zephyr 4.4.1, after migrating ZMK's kscan and USB integrations as described below; this is the recommended baseline for the new port.
2. Keep all USB-specific S3 work behind a separate configuration/CI target, even after the upgrade.
3. Use the existing 4.1 fork only for an interim BLE proof of concept. If S3 USB is urgently required before the upgrade, a restricted temporary backport may include only its DWC2 binding, SoC node, board enablement, and proven dependencies; remove it when upgrading.
4. Pin any selected newer revision to a reviewed commit, not an unbounded `main` branch. Espressif recommends current commits during development, but ZMK needs reproducible builds and must carry its own Zephyr compatibility fixes.

## Upgrade assessment: Zephyr 4.1 vs newest stable Zephyr 4.4

**Recommendation:** make a repository-wide upgrade to **Zephyr 4.4.1** the preferred route if S3 wired USB HID is a project goal. It is the newest stable release at the time of this report (4.5 is still a working draft), is supported upstream until 2027-04-12, and includes the S3 USB-OTG description which 4.1 lacks. Its bug-fix release also includes Espressif Bluetooth qualification, an ESP32-S3+iOS reconnect fix, and S3 PSRAM fixes. Do **not** upgrade only the Espressif board definitions: Zephyr and its `hal_espressif` revision are a coupled set, and this repository has a ZMK-specific Zephyr fork that must be rebased/ported as a whole.

### Benefits of upgrading

| Area | 4.1 current baseline | 4.4 benefit |
| --- | --- | --- |
| ESP32-S3 USB | No S3 `usb_otg` / DWC2 controller node in the 4.1 SoC DTS; wired ZMK HID is blocked. | The S3 DTS defines `usb_otg` (`espressif,esp32-usb-otg`, DWC2), enabling driver validation. SuperMini USB HID remains conditional on its USB-PHY/eFuse gate. |
| Espressif platform maturity | Older ESP-IDF-derived HAL/blob revisions and early 4.x platform support. | Updated Zephyr and `hal_espressif` revisions, plus continued Espressif SoC/driver fixes. This is particularly valuable for an out-of-tree C3/S3 port where storage, Bluetooth, flashing, and power behavior must be characterized anyway. |
| Bluetooth HCI correctness | Espressif binding defaults `bt-hci-bus` to deprecated `ipm`. | The 4.4 Espressif binding uses `ipc`, matching removal of `ipm` as a valid HCI bus value. New C3/S3 variants start from the current binding semantics. |
| Security and maintenance | Older release series with a growing delta from supported upstream. | 4.4 includes accumulated fixes/security advisories and has a stated upstream maintenance window through 2027-04-12. |
| Tooling/diagnosis | Standard build output. | 4.4 adds a build dashboard with footprint, devicetree, and initialization information—useful when tuning constrained C3/S3 RAM and Bluetooth heap usage. |
| Future cost | A local 4.1 S3 USB backport would create a maintenance fork. | Avoids carrying the DWC2/S3 platform backport and starts the port from the newer supported hardware model. |

These benefits do **not** establish low-power keyboard suitability: newer Zephyr makes the hardware features available, but battery current, BLE reliability, and settings persistence still require hardware measurements.

### Disadvantages and migration work

This is a significant **ZMK-wide** migration, not an ESP-only board addition:

| Cost/risk | Evidence in this repository or upstream | Required response |
| --- | --- | --- |
| ZMK's legacy keyboard scan architecture must change | Zephyr removed the whole kscan subsystem in 4.2. This tree retains seven legacy kscan translation units under `app/module/drivers/kscan/`, plus `kscan.h` consumers and bindings. | Complete and test the scan-driver migration across all supported boards before or as part of the Zephyr upgrade; do not mask it with compatibility copies of removed Zephyr APIs. |
| ZMK's legacy USB stack must change | `app/Kconfig`, `app/src/usb.c`, and `app/src/usb_hid.c` use `CONFIG_USB_DEVICE_STACK`, `usb_enable()`, and legacy HID APIs. They remain present but are marked deprecated in Zephyr 4.4.1. | Treat the USB migration as a first-class workstream. Port ZMK HID, endpoint/activity handling, Studio UART where applicable, and all USB board defaults; then add S3 DWC2 support. |
| Toolchain/CI floor rises | Zephyr 4.4 requires Zephyr SDK >= 1.0.0, Python >= 3.12, and defaults to C17. | Update devcontainers, GitHub Actions, setup docs, cache keys, compiler/linter assumptions, and contributor guidance before changing the manifest pin. |
| Board and devicetree compatibility changes | 4.2 removes HWMv1; 4.4 requires `full_name` for new board.yml entries, changes code-partition guidance, and updates several APIs/bindings. Existing ZMK is already HWMv2 because of its 4.1 update, but every in-tree/out-of-tree integration still needs a build audit. | Run all board/shield CI builds, apply upstream migration guides cumulatively (4.2, 4.3, 4.4), and publish an out-of-tree module migration guide. |
| Storage/include changes | 4.4 moves NVS/ZMS headers from `zephyr/fs/` to `zephyr/kvss/`. | Search and update ZMK and its modules; perform settings/bond compatibility and endurance testing. |
| Bluetooth API/configuration drift | 4.2 changes HCI buffer encoding; 4.4 removes the `ipm` HCI bus property and makes additional Bluetooth API cleanups. | Rebuild ZMK BLE/split/studio paths and test all host profiles; confirm Espressif HCI works with its matching 4.4 HAL/blobs. |
| Broader regression blast radius | ZMK supports many non-Espressif boards, displays, sensors, split transports, bootloaders, and Studio. | Use staged PRs and a full board/shield build matrix. The ESP port must not become the justification for accepting unrelated regressions. |

### Recommended upgrade sequence

1. **Create an upgrade branch before ESP board work.** Rebase the ZMK Zephyr fork/fixes on exactly `v4.4.1`, import the matching upstream manifest revisions—especially `hal_espressif`—and retain only necessary ZMK patches.
2. **Modernize ZMK first:** migrate every in-tree kscan driver to matrix input and migrate legacy USB device/HID usage to Zephyr's new USB stack. Preserve behavior through unit tests and hardware smoke tests on representative Nordic, RP2040, STM32, and split boards.
3. **Update the environment and CI:** Zephyr SDK 1.0, Python 3.12, C17, west/pip dependencies, all manifest/cache rules, and generated board metadata validation.
4. **Run the full existing regression matrix**, then add compile-only C3 and S3 BLE targets. Use the real Espressif blobs and boards for functional validation.
5. **Add S3 USB HID only after the generic USB migration passes.** Enable `usb_otg`/`zephyr_udc0`, validate enumeration and HID reports, then test BLE/USB switching.
6. **Keep C3 BLE-only.** The upgrade does not change its physical absence of USB OTG/HID capability.

### Decision matrix

| Project priority | Best choice |
| --- | --- |
| Fastest BLE proof of concept, minimal disruption | Remain on ZMK's 4.1 fork; add C3/S3 BLE variants; defer S3 USB. |
| Long-term C3/S3 support, especially S3 wired HID | Upgrade the whole project to Zephyr 4.4 first, then add BLE and USB variants. **Recommended.** |
| Need S3 USB now but cannot perform the ZMK migration | Carry a narrowly scoped 4.1 S3 USB backport as a temporary experimental branch only. This has the highest maintenance risk and should not be the default support path. |

## Proposed implementation design

### Phase 0 — prerequisites and acceptance definition

* Obtain exact C3/S3 SuperMini reference boards and record their chip markings, flash/PSRAM, USB route, BOOT/reset/LED wiring, and PCB revision before defining support.
* Establish a minimal matrix keyboard fixture, UART/USB-serial logging path, a Linux/macOS/Windows BLE host matrix, and a power meter.
* Define initial support as: matrix scanning, BLE HID keyboard, pairing/bonds/profiles retained across reset, reset behavior, reflash/recovery, and stable reconnect. Exclude battery claims, wireless split, RGB/display, deep sleep, and USB from the initial C3 milestone.

### Phase 1 — BLE-only ZMK board variants (C3, then S3)

Add the same board-variant files used by existing ZMK boards under `app/boards/espressif/<board>/`:

1. `board.yml` extending the upstream Zephyr board and declaring `zmk` with the exact SoC qualifier.
2. A `*_zmk_defconfig` that enables GPIO and `CONFIG_ZMK_BLE=y`, disables `CONFIG_ZMK_USB`, enables writable flash/settings, and provides conservative Bluetooth buffer/connection defaults only when measurements justify them.
3. A `*_zmk.dts` that includes the upstream board DTS, disables unnecessary console nodes where they conflict with keyboard pins or power goals, and defines/overrides a **dedicated writable storage partition**. Retain the upstream BT HCI chosen node and the enabled `esp32_bt_hci` node.
4. A minimal in-tree shield/fixture or sample configuration with a small direct GPIO matrix. Do not bake a physical keyboard's matrix pins into a generic development-board definition.

Settings storage needs an explicit decision after testing:

* First attempt NVS + `SETTINGS_NVS` with flash map/page layout, as used by current ZMK boards.
* Verify erased-flash boot, save/load across reset and power cycle, bond overwrite/clear, and repeated writes.
* If NVS fails on hardware, use FCB only as an explicitly documented temporary fallback and open an upstream Zephyr/Espressif issue with a minimal reproducer. Do not silently ship volatile profiles.

### Phase 2 — validation and configuration sizing

* Fetch `hal_espressif` blobs and test actual firmware rather than only a `BUILD_ONLY_NO_BLOBS` build.
* Start with single-peripheral ZMK HID. Test multiple profiles and the full bond lifecycle before enabling split Bluetooth.
* Capture controller/host memory consumption after `bt_enable()`. The Espressif driver reserves a 25,600-byte heap addition by default and a 4,096-byte controller task stack, which is meaningful on 400/512 KB SRAM devices. Tune only after measuring high-water marks and reconnection reliability.
* Exercise keyboard typing, NKRO/consumer reports, pairing, reconnect after host sleep, profile switch, bond clear/reset, and long-duration connected-idle testing on every supported host OS.
* Measure current in active scanning/advertising, connected idle at ZMK's preferred latency, key bursts, deep/light sleep attempts, and powered-off/reset states. Publish the test circuit, firmware SHA, radio settings, sample size, and median/peak current.

### Phase 3 — split Bluetooth (optional, experimental)

ZMK's existing split BLE code is SoC-neutral, and the historical S3 proof of concept reported split operation. Nevertheless, validate it after single-board BLE because it stresses central and peripheral roles, scans, reconnects, settings persistence, and radio coexistence. Start with S3 (more RAM), a two-half fixed fixture, no display/RGB, and explicit throughput/latency tests. Do not advertise it as production-ready until disconnect recovery survives extended tests.

### Phase 4 — S3 wired USB HID

After the Zephyr upgrade/backport decision:

1. Create an S3-only USB-enabled board/shield configuration; C3 remains BLE-only.
2. Enable the S3 OTG UDC/DWC2 controller and ZMK USB HID, then prove enumeration and report delivery on all host OSes.
3. Test power/boot interactions: serial/JTAG versus OTG pins, VBUS behavior, bootloader/download mode, reset after flash, cable reconnect, and BLE + USB endpoint switching.
4. Add regression builds and physical smoke testing before declaring support.

## Risks and mitigations

| Risk | Impact | Mitigation / release gate |
| --- | --- | --- |
| High idle current or incomplete sleep | Battery keyboard not viable | Measure before product claims; initially support externally powered BLE keyboards only. |
| BLE regressions/interop instability | Core keyboard failure | Long reconnect, bond, and host interoperability tests; track upstream Espressif BLE fixes. |
| Proprietary RF blob availability/version coupling | Build or runtime failure | Document blob fetch; pin Zephyr/HAL revisions together; CI source-only build is not sufficient. |
| Flash settings corruption or NVS failure | Lost profiles/bonds | Dedicated partition; endurance and reset tests; use documented FCB fallback only with upstream issue. |
| S3 USB API/platform evolution | Wired HID delayed | Isolate behind S3-only configuration; base on Zephyr >= 4.4 and test the actual ZMK legacy USB HID stack. |
| Pin/flash module variation | Unflashable or non-booting user hardware | Begin with official upstream boards; model module flash size and reserved pins explicitly; add custom boards separately. |
| Dual-core S3 complexity | Debugging/memory/radio risk | Use only PROCPU for ZMK. No SMP/AMP in initial scope. |

## CI and documentation additions once implementation starts

* Add compile-only matrix entries for C3 BLE and S3 BLE; add S3 USB only after the Zephyr baseline supports it.
* Run `west blobs fetch hal_espressif` in hardware-capable developer setup, but do not require proprietary blobs for generic source CI unless licensing/CI policy allows it.
* Publish flashing and recovery instructions using Espressif's runner (`west flash`), including the known possibility that USB Serial/JTAG targets require a reset/power cycle after flashing.
* Document qualified module flash sizes, unavailable C3 USB HID, expected power status, and whether split is experimental.
* Add hardware test results—not merely successful compilation—to the support declaration.

## Go / no-go criteria

**Go for BLE-first experimental support** when C3 and S3 each pass build, flash, matrix, bond persistence, multi-profile, and 24-hour reconnect/typing tests with documented current draw.

**Go for general support** only when power data is acceptable for the stated use case and BLE reliability passes the host matrix. Keep wireless split experimental until separately qualified.

**Go for S3 USB support** only after ZMK builds against the selected newer Zephyr baseline and real hardware enumerates as a USB HID keyboard reliably. There is no equivalent USB-HID milestone for C3.

## Sources

* Current ZMK Zephyr pin: [`app/west.yml`](app/west.yml).
* ZMK output selection and BLE defaults: [`app/Kconfig`](app/Kconfig); implementation: [`app/src/ble.c`](app/src/ble.c).
* Zephyr 4.1 C3 board: <https://docs.zephyrproject.org/4.1.0/boards/espressif/esp32c3_devkitm/doc/index.html>
* Zephyr 4.1 S3 board: <https://docs.zephyrproject.org/4.1.0/boards/espressif/esp32s3_devkitc/doc/index.html>
* Zephyr 4.4 C3 SuperMini board (the direct upstream base): <https://docs.zephyrproject.org/4.4.0/boards/others/esp32c3_supermini/doc/index.html>.
* Zephyr 4.1 Espressif HCI configuration: <https://github.com/zephyrproject-rtos/zephyr/blob/v4.1.0/drivers/bluetooth/hci/Kconfig> and driver: <https://github.com/zephyrproject-rtos/zephyr/blob/v4.1.0/drivers/bluetooth/hci/hci_esp32.c>.
* Zephyr 4.4 S3 USB controller DTS: <https://github.com/zephyrproject-rtos/zephyr/blob/v4.4.0/dts/xtensa/espressif/esp32s3/esp32s3_common.dtsi> and DevKitC enablement: <https://github.com/zephyrproject-rtos/zephyr/blob/v4.4.0/boards/espressif/esp32s3_devkitc/esp32s3_devkitc_procpu.dts>.
* Zephyr 4.4.1 bug-fix release and 4.4 migration guide: <https://github.com/zephyrproject-rtos/zephyr/releases/tag/v4.4.1> and <https://docs.zephyrproject.org/4.4.0/releases/migration-guide-4.4.html>. Apply the cumulative 4.2/4.3 guides as well.
* Espressif Zephyr support status (updated 2025-10-20): <https://developer.espressif.com/software/zephyr-support-status/>.
* Upstream Espressif status matrix / USB and low-power status: <https://github.com/zephyrproject-rtos/zephyr/issues/29394>.
* ZMK's Zephyr 4.1 update and announced next-upgrade work (matrix input and new USB stack): <https://zmk.dev/blog/2025/12/09/zephyr-4-1>.
* Upstream kscan deprecation/removal: <https://github.com/zephyrproject-rtos/zephyr/commit/f7c0a643dfd4d5b535de9b515ec98d1a7236cda4> and <https://github.com/zephyrproject-rtos/zephyr/commit/60a9a202df6fff71955b3f295d7f172e1ab0615e>.
* Zephyr 4.4.1 Input keyboard-matrix implementation: <https://github.com/zephyrproject-rtos/zephyr/blob/v4.4.1/drivers/input/input_kbd_matrix.c>; legacy USB is marked deprecated in <https://github.com/zephyrproject-rtos/zephyr/blob/v4.4.1/subsys/usb/device/Kconfig> and <https://github.com/zephyrproject-rtos/zephyr/blob/v4.4.1/include/zephyr/usb/usb_device.h>.
* Espressif S3 USB PHY/OTG caveat: <https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-guides/usb-otg-console.html>; C3 fixed-function USB Serial/JTAG: <https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-guides/usb-serial-jtag-console.html>.
* SuperMini reference pinout research (verify against the physical board): <https://lastminuteengineers.com/esp32-c3-super-mini-pinout-reference/> and <https://esp32.co.uk/esp32-s3-supermini-pinout-safe-gpios-usb-psram/>.
* Historical ZMK ESP32-S3 proof of concept and limitations: <https://github.com/zmkfirmware/zmk/issues/2173>.
* Example upstream BLE instability report (closed; use as a regression-test prompt, not proof that C3/S3 are affected): <https://github.com/zephyrproject-rtos/zephyr/issues/87621>.