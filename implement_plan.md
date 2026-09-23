# Implementation plan: Zephyr 4.4 upgrade and ESP32-C3/ESP32-S3 ZMK support

## Purpose and constraints

Deliver supported ZMK board variants for the SuperMini ESP32-C3 and ESP32-S3, with:

* **C3:** BLE HID keyboard support only.
* **S3:** BLE HID first; wired USB HID only after the generic ZMK USB migration **and** a board-specific USB-PHY/eFuse decision gate.
* A whole-project Zephyr **4.4.1** upgrade before permanent ESP support.

This plan intentionally contains **no implementation**. Each phase should land as reviewable commits/PRs with its validation gate satisfied before starting the next phase.

## Non-goals for the first release

* ESP32-C3 USB HID (not supported by the chip hardware).
* Battery-life claims or production battery-powered support.
* ESP32-S3 SMP/AMP or execution on APPCPU.
* Wi-Fi features.
* Wireless split support, displays, RGB, pointing, and Studio over ESP32 until single-board BLE is stable.
* A permanent downstream backport of S3 USB support to Zephyr 4.1.
* Burning `USB_PHY_SEL` or any other eFuse on a normal development/CI board. Any fused USB experiment requires a documented, sacrificial-board procedure and explicit project approval.

## Target definition

| Item | C3 target | S3 target |
| --- | --- | --- |
| Initial board | Upstream `esp32c3_supermini` | New ZMK `esp32s3_supermini` board, qualified against a specific physical revision |
| Zephyr build target | `esp32c3_supermini/esp32c3` | `esp32s3_supermini/esp32s3/procpu` |
| Architecture | RISC-V, single-core | Xtensa, use PROCPU only |
| Initial ZMK output | BLE HID | BLE HID |
| Deferred output | N/A | USB HID via native OTG/DWC2 only if the USB-PHY gate passes |
| Qualified reference | Common C3FX4/FN4, 4-MB SuperMini; verify silkscreen | Common S3FH4R2, 4-MB flash/2-MB QSPI PSRAM SuperMini; verify chip marking and USB wiring |

## Delivery sequence

```text
0. Baseline and hardware fixtures
1. Upgrade ZMK’s Zephyr baseline to 4.4.x
2. Migrate legacy kscan to matrix input
3. Migrate legacy USB device/HID stack
4. Full non-ESP regression gate
5. Add C3 BLE board variant
6. Add S3 BLE board variant
7. ESP BLE qualification and power characterization
8. Add and qualify S3 USB HID
9. Optional split BLE and advanced peripherals
10. Documentation, CI policy, and support declaration
```

Phases 1--4 are deliberately ahead of ESP board code. The 4.4 upgrade is not safe to combine with untested ESP platform work because regressions would be difficult to attribute.

---

## Phase 0 — establish a reproducible baseline

### Tasks

1. Create an upgrade/port tracking branch and record the current ZMK commit, current `v4.1.0+zmk-fixes` base, active ZMK Zephyr patch series, and all west module revisions.
2. Acquire two known-good development boards, USB cables, a serial monitor path, a 3.3-V power supply, and a current meter/power profiler.
3. Build a simple direct-GPIO keyboard matrix fixture. It must expose row/column pins without conflict with flash, boot strapping, UART/JTAG, onboard LEDs, or native USB pins.
4. Photograph and record the exact silkscreen, chip marking, PCB revision, flash size, PSRAM size, BOOT/RESET/LED wiring, and USB-C D+/D- route for every reference board. "SuperMini" alone is not a sufficient hardware identifier.
5. Confirm C3 USB-C is USB Serial/JTAG on GPIO18/19 and confirm S3 USB-C is routed to GPIO19/20. Do not assume that S3's USB-C connector is immediately usable by the programmable OTG controller.
6. Prepare host test machines or repeatable virtual/physical test procedures for Linux, macOS, Windows, Android, and iOS where available.
7. Record baseline CI results, release build sizes, and test coverage for existing ZMK boards before changing the Zephyr pin.
8. Define a test-results format with: board/module revision, chip marking, flash/PSRAM size, USB-PHY state/eFuse state, firmware SHA, Zephyr/hal_espressif SHA, toolchain versions, host OS, test case, duration, result, logs, and measured current.

### Deliverables

* A reproducible hardware test fixture description.
* A compatibility matrix and issue tracker/checklist.
* Archived baseline build and test output.

### Exit gate

Current mainline builds and tests are green, and at least one developer can flash/monitor each ESP development board independently.

---

## Phase 1 — upgrade the Zephyr platform to 4.4.x

### Tasks

1. Pin Zephyr to `v4.4.1`, the newest stable 4.4 bug-fix release. Do not pin to `main`.
2. Rebase ZMK’s fork/patches currently referenced by `app/west.yml` onto that selected upstream commit. Classify every existing patch as:
   * upstream already contains it;
   * still needed and cleanly applicable;
   * needs a port; or
   * obsolete/replaced.
3. Update `app/west.yml` to pin the rebased Zephyr revision and import the matching Zephyr west dependencies. In particular, consume Zephyr 4.4’s matching `hal_espressif` revision rather than mixing a 4.1 HAL with 4.4 Zephyr.
4. Update any ZMK-pinned module revisions which must match the new Zephyr API (for example LVGL and other imported modules). Do not update unrelated modules merely because newer versions exist.
5. Update development environment definitions and CI:
   * `.devcontainer/` image/toolchain setup;
   * GitHub workflow Python installation and cache keys;
   * Zephyr SDK >= 1.0.0;
   * Python >= 3.12;
   * C17-compatible compiler flags/toolchain;
   * `west` and required Python packages.
6. Run `west update`, `west blobs fetch hal_espressif`, and `west packages pip --install` in a clean workspace. Confirm both source-only and blob-backed ESP workflows are documented.
7. Apply the Zephyr migration guides cumulatively: 4.2, 4.3, and 4.4. Track each change against this repository rather than assuming it is irrelevant.

### Likely files/directories to inspect or modify

* `app/west.yml`
* `.devcontainer/`
* `.github/workflows/`
* CI/container documentation and local toolchain documentation
* The downstream Zephyr fork/repository and its patch metadata (if maintained outside this checkout)

### Validation

* Configure and build a representative USB+BLE Nordic board, an RP2040 board, an STM32 board, a native test board, and a split board.
* Ensure the resolved west manifest contains the expected Zephyr and `hal_espressif` SHAs.
* Confirm no stale toolchain/Python dependency is hidden by a warm CI cache.

### Exit gate

The Zephyr 4.4 manifest resolves reproducibly; representative builds pass; all downstream Zephyr fixes are either ported, upstreamed, or consciously dropped with a recorded rationale.

---

## Phase 2 — migrate ZMK keyboard scanning from legacy kscan

### Why this is separate

This is a hard migration gate, not cleanup: upstream removed the complete kscan subsystem in Zephyr 4.2. Zephyr 4.4.1 has no `zephyr/drivers/kscan.h`, `CONFIG_KSCAN`, `kscan_driver_api`, or upstream kscan bindings/drivers. This repository has seven legacy kscan translation units under `app/module/drivers/kscan/` and legacy consumers in physical-layout/sideband code. Carrying a private compatibility copy would create a permanent, unmaintained fork of a removed subsystem.

### Tasks

1. Inventory all use of `zephyr/drivers/kscan.h`, `CONFIG_KSCAN`, kscan devicetree bindings, callbacks, and test fixtures. Include `app/src/physical_layouts.c`, `app/src/kscan_sideband_behaviors.c`, `app/include/zmk/physical_layouts.h`, `app/include/zmk/matrix.h`, and all board/shield overlays.
2. Make the existing Input path in `app/src/physical_layouts.c` the sole path: producers report `INPUT_ABS_X`, `INPUT_ABS_Y`, then a synchronized `INPUT_BTN_TOUCH`; the listener maps that triplet through the active layout's matrix transform. Replace callback enable/disable with tested device-power/layout filtering behavior.
3. Port one driver at a time, beginning with the direct GPIO matrix fixture, then the common matrix/direct/charlieplex/demux/composite drivers, and finally mocks/test-only drivers.
4. Preserve behavior relevant to keyboards:
   * debounce;
   * diode direction;
   * wake-up interrupt behavior;
   * scan timing and idle state;
   * ghosting semantics;
   * sideband behaviors;
   * split-half roles.
5. Convert or replace associated devicetree bindings and overlays. Remove legacy Kconfig selection only after all consumers are migrated.
6. Expand unit/native tests so the three-event ordering and synchronization boundary, press/release order, debounce, simultaneous presses, and sleep/wake events are asserted rather than compile-tested only.

### Likely files/directories

* `app/module/drivers/kscan/`
* `app/module/drivers/CMakeLists.txt`
* `app/module/drivers/kscan/Kconfig`
* `app/src/kscan_sideband_behaviors.c`
* `app/src/main.c`, input/event plumbing, and related headers
* `app/dts/bindings/` and `app/boards/**` overlays that instantiate ZMK kscan nodes
* `app/tests/`

### Validation

* Native/unit tests for each driver.
* Physical smoke tests on at least one board per driver family.
* Build every in-tree board/shield that uses each affected driver.
* Verify a held key through USB and BLE, wake from idle, and matrix rollover behavior.

### Exit gate

No production ZMK driver depends on the removed kscan API. Existing keyboard scan behavior has matching or stronger automated and physical coverage.

---

## Phase 3 — migrate ZMK USB HID to the current Zephyr USB stack

### Why this is separate

The S3 OTG/DWC2 platform is useful only after ZMK can provide HID on the modern USB device stack. Unlike kscan, the legacy USB stack still exists in 4.4.1, but it selects `DEPRECATED`; `usb_enable()`, legacy descriptors, and HID APIs are explicitly deprecated. Do not use its continued compilation as evidence that it is a supportable S3 baseline.

### Tasks

1. Inventory all legacy USB references and all user-facing USB functions: keyboard/consumer/mouse reports, boot protocol, endpoint switching, USB connection status, power/activity state, display status widgets, USB logging, and Studio UART transports.
2. Design a small ZMK USB abstraction that keeps endpoint/report callers independent of the Zephyr USB stack API. Avoid a broad rewrite of HID report generation.
3. Implement an explicit USBD context bound to `zephyr_udc0`, with descriptors, speed-specific configuration/class registration, initialization, enable/disable, and lifecycle notifications via `usbd_msg_register_cb()`.
4. Implement the modern USB-device/HID report submission path for keyboard, consumer, and mouse reports; port HID protocol/idle callbacks, suspend/resume, remote wakeup, and connection-state event propagation.
5. Port optional USB CDC/Studio/logging configurations separately from HID so disabling them does not break basic keyboards.
6. Update board defaults and Kconfig dependencies from the legacy USB stack to the new USB device/controller model.
7. Test with controllers already known to work in ZMK before introducing S3: at minimum one Nordic, RP2040, and STM32 USB board.

### Likely files/directories

* `app/Kconfig`
* `app/src/usb.c`
* `app/src/usb_hid.c`
* `app/src/endpoints.c`
* `app/src/activity.c`
* `app/src/events/usb_conn_state_changed.c`
* `app/src/studio/`
* display/widget files guarded by `CONFIG_USB_DEVICE_STACK`
* board `Kconfig.defconfig`, `*_zmk_defconfig`, DTS, and shield `.conf` files

### Validation

* USB enumeration and report delivery on Linux, macOS, and Windows.
* Keyboard boot/report protocol switching, consumer and mouse reports, suspend/resume, unplug/replug, and reset while connected.
* Existing USB logging and Studio configurations, where supported.
* Build and flash size comparison against 4.1; document material regressions.

### Exit gate

Legacy USB stack symbols/APIs no longer underpin supported ZMK USB HID. The modern path passes regression tests on all representative USB controllers.

---

## Phase 4 — whole-project upgrade regression gate

### Tasks

1. Run the complete automated suite, all configured CI build targets, devicetree/hardware metadata validation, formatting, and static checks.
2. Produce a board-by-board migration failure list. Fix or explicitly quarantine only boards already unsupported upstream; do not silently reduce claimed support.
3. Run physical smoke tests for representative combinations:
   * BLE-only board;
   * USB+BLE board;
   * wired split;
   * wireless split;
   * display;
   * RGB/LED;
   * encoder/pointing input;
   * settings reset and retained boot behavior.
4. Validate persistent settings format behavior across firmware upgrade/downgrade for representative NVS/FCB users. Clearly document any required settings reset.
5. Review resulting binary RAM/flash changes and stack high-water marks for constrained existing boards.

### Exit gate

The Zephyr upgrade can stand on its own as a release candidate. Only then should ESP support merge on top of it.

---

## Phase 5 — add the ESP32-C3 SuperMini BLE board variant

### Design

Use Zephyr 4.4's upstream `esp32c3_supermini/esp32c3` board as a board extension/variant. Do not copy its SoC or board definition. The ZMK extension supplies only ZMK-specific defaults, flash settings storage, and output policy. Note that upstream labels this third-party board **not actively maintained**, so the selected physical reference revision must be qualification-tested by ZMK.

### Tasks

1. Add a ZMK board extension for the upstream board following the Zephyr 4.4 board-extension convention. Do not redefine `board.yml` because upstream already declares the board/SoC.
2. Add the C3 ZMK defconfig:
   * enable GPIO and `CONFIG_ZMK_BLE=y`;
   * keep ZMK USB disabled;
   * enable flash map/page layout and a candidate settings backend;
   * retain normal Zephyr Bluetooth host configuration and Espressif HCI selection;
   * do not guess Bluetooth buffer/stack tuning before measurements.
3. Add the C3 ZMK DTS extension:
   * include/extend the upstream SuperMini DTS;
   * preserve `zephyr,bt-hci = &esp32_bt_hci` and enabled HCI node;
   * add a non-overlapping writable storage partition sized for ZMK settings;
   * disable or repurpose console/UART only if it conflicts with the matrix fixture;
   * make flash-size/module assumptions explicit.
4. Add a minimal direct matrix shield/fixture configuration, separate from the generic board variant. Its default pins must not use GPIO18/19 (USB Serial/JTAG), GPIO8 (onboard active-low LED), GPIO9 (BOOT), or strapping pins GPIO2/8/9. Prefer GPIO0, GPIO1, GPIO3--GPIO5, GPIO10, GPIO20, and GPIO21 subject to the qualified carrier schematic.
5. Disable UART console if GPIO20/21 become matrix pins; retain USB Serial/JTAG console for flash/recovery.
6. Add C3 compile targets to CI. Configure blob-backed hardware tests outside generic CI if secrets/licenses prohibit blob fetching there.

### Proposed paths (confirm against the migrated tree)

* `app/boards/extensions/others/esp32c3_supermini/esp32c3_supermini_esp32c3_zmk_defconfig`
* `app/boards/extensions/others/esp32c3_supermini/esp32c3_supermini_esp32c3_zmk.dts`
* `app/boards/shields/<esp32_fixture>/`
* `.github/workflows/` or the project’s board-test metadata location

Exact paths/names must follow the 4.4 board-extension rules verified in Phase 1.

### C3 functional tests

1. Build cleanly with blobs fetched; flash using the Espressif runner; verify boot logs.
2. Scan fixture matrix and send keys over BLE.
3. Pair and type on Linux, macOS, Windows, Android, and iOS as available.
4. Verify all ZMK profile operations: first pair, profile selection, reconnect, clear one bond, clear all bonds, settings reset, and power-cycle persistence.
5. Test erased-flash first boot, 100+ repeated settings saves, unexpected reset during use, and reflash recovery.
6. Run 24-hour connected idle and repeated reconnect tests, recording disconnect reason/logs.
7. Confirm that enabling ZMK USB is rejected/unsupported in documentation and CI.

### Exit gate

C3 BLE HID works reliably on the supported host matrix and profile persistence works with the chosen storage backend. It is marked experimental until Phase 7 power criteria are met.

---

## Phase 6 — add the ESP32-S3 SuperMini BLE board and ZMK variant

### Design

No upstream Zephyr 4.4 S3 SuperMini board exists, so this phase creates a complete board definition and its `zmk` variant. Use only `esp32s3/procpu`; the application is single-core and does not introduce SMP/AMP. Qualify against a concrete common board revision, initially an `ESP32-S3FH4R2` (4-MB flash, 2-MB quad-SPI PSRAM) sample. Do not claim compatibility with other SuperMini clones until their chip marking and wiring are tested.

### Tasks

1. Add `board.yml` with `full_name`, board vendor/category, the `esp32s3` SoC, the `procpu` qualifier, and the ZMK variant.
2. Add the base board DTS, beginning with Zephyr's `espressif/esp32s3/esp32s3_wroom_n4r2.dtsi` **only after** confirming the reference chip's 4-MB flash/2-MB QSPI PSRAM topology. Set the board flash/PSRAM properties explicitly and use an Espressif 4-MB partition include as the starting layout.
3. Add S3 pinctrl and board-CMake/runner configuration based on current Zephyr S3 boards; configure USB Serial/JTAG console for normal development and preserve the Espressif HCI chosen node.
4. Add the `zmk` variant defconfig and DTS: enable BLE/GPIO/settings, disable ZMK USB, create a dedicated writable settings partition, and keep Wi-Fi disabled.
5. Add an S3 direct matrix fixture configuration. Default matrix pins are GPIO1, GPIO2, GPIO4--GPIO8, GPIO15--GPIO18, and GPIO21. Reserve GPIO0, GPIO3, GPIO19/20, GPIO45/46, and GPIO48; retain GPIO43/44 as UART recovery until USB behavior is qualified. Do not use GPIO33--38 in the portable default fixture.
6. Leave PSRAM unused by default. Add PSRAM configuration only after a dedicated memory/boot validation on the exact FH4R2 board.
7. Add S3 BLE compilation to CI and hardware qualification to the test matrix.

### Proposed paths (confirm after Phase 1)

* `app/boards/others/esp32s3_supermini/board.yml`
* `app/boards/others/esp32s3_supermini/esp32s3_supermini_esp32s3_procpu.dts`
* `app/boards/others/esp32s3_supermini/esp32s3_supermini_esp32s3_procpu-pinctrl.dtsi`
* `app/boards/others/esp32s3_supermini/esp32s3_supermini_esp32s3_procpu_defconfig`
* `app/boards/others/esp32s3_supermini/esp32s3_supermini_esp32s3_procpu_zmk_defconfig`
* `app/boards/others/esp32s3_supermini/esp32s3_supermini_esp32s3_procpu_zmk.dts`
* required board CMake/Kconfig/sysbuild metadata consistent with Zephyr 4.4 Espressif boards

### Exit gate

S3 passes the same BLE functional and persistence suite as C3, with PROCPU-only operation documented.

---

## Phase 7 — ESP BLE reliability and power qualification

### Tasks

1. Instrument C3 and S3 builds to capture Bluetooth/controller initialization, heap allocation failure, settings write failures, HCI errors, disconnect reasons, and reset causes.
2. Measure memory after boot and after `bt_enable()`, during pairing, active typing, split experiments, and after settings writes. Record stack high-water marks.
3. Tune only measurements-proven values, including Espressif controller task stack, Bluetooth host buffers, ZMK BLE report queues, connection intervals, and preferred peripheral latency.
4. Measure current at 3.3 V for:
   * boot;
   * open advertising;
   * connected idle;
   * key burst;
   * reconnect;
   * light/deep sleep attempts;
   * settings writes; and
   * powered-off/soft-off state.
5. Run at least 24-hour idle/reconnect tests and a multi-day typing/reconnect soak test for each chip.
6. Test difficult host behavior: host sleep/wake, Bluetooth toggled off/on, profile handoff, bond overwrite, host out of range, and radio congestion.

### Acceptance criteria

Set numeric current and reliability targets before tests begin. Initial criteria should include no unhandled reset, no persistent pair/bond corruption, recovery without reflashing after host disconnects, and reproducible logs for all failures. Do not label a battery configuration supported until its measured current meets a published use-case budget.

### Exit gate

Publish measured power/reliability data. Decide separately whether each board is experimental, externally powered only, or suitable for the stated battery product.

---

## Phase 8 — investigate, then conditionally enable, ESP32-S3 SuperMini USB HID

### Preconditions

* Phases 1--4 completed on Zephyr 4.4.x.
* ZMK modern USB migration completed.
* S3 BLE board variant stable.
* The exact SuperMini USB-C wiring has been inspected and documented.
* The project has decided whether it accepts the irreversible `USB_PHY_SEL` eFuse change required to connect the S3's internal PHY to OTG when no external PHY exists.
* A UART0 (GPIO43/44) recovery procedure and a sacrificial test board are available.

### Tasks

1. Inspect the SuperMini schematic and continuity-test USB-C D+/D- to GPIO19/20. Determine whether an external PHY exists. Record eFuse state before every test.
2. If an external PHY path exists, add an S3-only USB configuration/variant that enables upstream `usb_otg`/`zephyr_udc0` and the migrated ZMK USB HID feature. Prove it without changing eFuses.
3. If only the internal PHY is available, stop before functional HID testing and obtain explicit approval for a **separate fused experimental variant**. Document that `USB_PHY_SEL` is permanent and changes/disables normal USB Serial/JTAG behavior.
4. On a sacrificial board only, apply the approved fuse procedure; validate ROM download recovery and UART0 recovery **before** attempting ZMK USB HID. Do not make this a CI or default-user requirement.
5. Add an explicitly named `usb_otg_fused` S3 configuration only if the fuse path proves repeatable. Keep the normal BLE-only/USB-Serial-JTAG S3 configuration unchanged.
6. Verify controller setup, VBUS/power requirements, descriptors, and endpoint allocation against S3 DWC2 capabilities.
7. Test USB-only, BLE-only while cabled, endpoint switching, cable reconnect, reset while attached, host suspend/resume, and recovery after malformed USB firmware.
8. Add S3 USB build coverage and a hardware smoke-test checklist; if possible, automate enumeration/report checks with a host-side test rig.

### Explicitly excluded

Do not expose this configuration for C3. Its USB Serial/JTAG peripheral is not an HID-capable OTG controller.

### Exit gate

Either (a) USB HID is rejected for the common SuperMini because it would require an unacceptable permanent fuse, in which case S3 remains BLE-only, or (b) the fused variant enumerates as a stable HID keyboard on Linux, macOS, and Windows; keyboard/consumer/mouse reports, suspend/resume, BLE/USB switching, and UART recovery all pass. USB support remains S3-specific, revision-specific, and explicitly fuse-qualified in documentation and CI.

---

## Phase 9 — optional capabilities (each independently experimental)

### Wireless split BLE

1. Start with S3 due to its greater RAM.
2. Use a fixed two-half fixture and validate central/peripheral reconnect, settings persistence, boot ordering, profile selection, and throughput under typing.
3. Run soak tests with one/both halves reset or out of range.
4. Qualify C3 only after S3 passes; do not assume parity.

### Battery, display, RGB, sensors, and pointing

Enable one peripheral class at a time. Each must demonstrate no unacceptable RAM/current impact and no interference with BLE/HID. Add board-specific overlays rather than expanding the generic development-board default.

### MCUboot / secure boot / OTA

Treat as a dedicated boot/partition project. Verify Espressif sysbuild/MCUboot images, partition layout, recovery, signing, and upgrade rollback before documenting support.

---

## Phase 10 — documentation, CI, and release policy

### Documentation tasks

1. Add hardware integration pages for C3 and S3 listing exact target names, compatible modules, flash size assumptions, pin reservations, matrix examples, flashing, serial monitor, reset/recovery, and blob fetch requirements.
2. Clearly state capability boundaries:
   * C3: BLE only; no USB HID.
   * S3: BLE; USB HID only on an explicit Zephyr-4.4-based, board-revision-specific, fuse-qualified variant if the USB-PHY gate passes.
   * split/power state: experimental until separately qualified.
3. Publish power methodology and results, not just a headline current number.
4. Document settings storage backend, partition size, reset behavior, and migration/reset requirements.
5. Document the Zephyr 4.4 upgrade impacts for out-of-tree boards/shields/modules, particularly matrix input and USB migration.

### CI tasks

1. Add compile targets for C3 BLE and S3 BLE. Add the S3 USB HID target only after the USB-PHY gate passes; label it fused/experimental in CI metadata.
2. Ensure west/module revision resolution is covered in clean CI environments.
3. Add automated tests for matrix input, BLE profile/settings logic, and USB HID abstraction where feasible.
4. Maintain hardware-in-loop/manual release checklists for flash, BLE, USB, settings, and power tests.

### Release gates

| Support level | Required evidence |
| --- | --- |
| Buildable | Clean CI compilation and generated artifacts. |
| Experimental BLE | Flash, matrix, pairing, profiles/settings persistence, and 24-hour reconnect test pass. |
| General BLE support | Full host matrix, multi-day soak, published power data, documented recovery path, and no known critical regressions. |
| S3 USB support | Modern ZMK USB stack, DWC2 hardware tests on Linux/macOS/Windows, and BLE/USB switching tests pass. |
| Split support | Independent two-half stress/recovery qualification. |

## Commit / PR boundaries

Keep commits independently reviewable and reversible:

1. Zephyr 4.4 manifest/fork/module upgrade only.
2. Toolchain/container/CI updates.
3. Matrix-input migration, split by driver family.
4. USB-stack migration, split by core HID and optional CDC/Studio.
5. Upgrade regression fixes and documentation.
6. C3 BLE board extension plus fixture and CI compile test.
7. S3 BLE board extension plus fixture and CI compile test.
8. ESP qualification results/documentation.
9. S3 USB variant and hardware test results.
10. Optional split/peripheral work, one feature at a time.

Never combine a Zephyr rebase, a broad API migration, and ESP board enablement in one change set.

## Risks that block progression

* If the new USB stack cannot meet existing ZMK HID/Studio needs, stop before S3 USB work; C3/S3 BLE work may continue on the upgraded baseline once the global regression gate passes.
* If NVS is unreliable on either ESP target, do not ship persistent profiles until FCB fallback is validated and an upstream minimal reproducer is filed.
* If BLE reconnect stability fails, do not progress to split; capture HCI and host logs, reduce the configuration to Zephyr samples, and resolve upstream/downstream ownership first.
* If current consumption is unsuitable, document an externally powered-only experimental configuration rather than marketing a battery keyboard.
* If the Zephyr 4.4 upgrade breaks supported non-ESP boards, resolve or explicitly defer the release; do not regress them to land ESP support.

## References

* Research summary and version rationale: [`report.md`](report.md).
* Current ZMK manifest: [`app/west.yml`](app/west.yml).
* ZMK BLE and USB implementation: [`app/Kconfig`](app/Kconfig), [`app/src/ble.c`](app/src/ble.c), [`app/src/usb.c`](app/src/usb.c), and [`app/src/usb_hid.c`](app/src/usb_hid.c).
* ZMK’s Zephyr 4.1 upgrade notice and next-upgrade prerequisites: <https://zmk.dev/blog/2025/12/09/zephyr-4-1>.
* Upstream kscan was removed in Zephyr 4.2: <https://github.com/zephyrproject-rtos/zephyr/commit/60a9a202df6fff71955b3f295d7f172e1ab0615e>. The Input keyboard-matrix event producer is <https://github.com/zephyrproject-rtos/zephyr/blob/v4.4.1/drivers/input/input_kbd_matrix.c>.
* Legacy USB is deprecated in 4.4.1: <https://github.com/zephyrproject-rtos/zephyr/blob/v4.4.1/subsys/usb/device/Kconfig> and <https://github.com/zephyrproject-rtos/zephyr/blob/v4.4.1/include/zephyr/usb/usb_device.h>. Current USB-device architecture: <https://docs.zephyrproject.org/4.4.0/connectivity/usb/device_next/usb_device.html>.
* Zephyr 4.4.1 release and 4.4 migration guide: <https://github.com/zephyrproject-rtos/zephyr/releases/tag/v4.4.1> and <https://docs.zephyrproject.org/4.4.0/releases/migration-guide-4.4.html>.
* Upstream C3 SuperMini board: <https://docs.zephyrproject.org/4.4.0/boards/others/esp32c3_supermini/doc/index.html>.
* Espressif S3 USB PHY/OTG behavior: <https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-guides/usb-otg-console.html>; C3 USB Serial/JTAG fixed-function behavior: <https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-guides/usb-serial-jtag-console.html>.
* SuperMini pinout references—verify against the actual shipped board: <https://lastminuteengineers.com/esp32-c3-super-mini-pinout-reference/> and <https://esp32.co.uk/esp32-s3-supermini-pinout-safe-gpios-usb-psram/>.