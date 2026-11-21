/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Coban Tab 1a storage/flash debug dump (workspace backlog c02).
 *
 * The `flash_esp32: Flash write error: 261` / `settings_subsys_init failed`
 * failure could not previously be reproduced without Bluetooth enabled,
 * because CONFIG_SETTINGS is only implied by ZMK_BLE. Kconfig
 * CONFIG_ZMK_TAB1A_DEBUG selects SETTINGS itself, so this module gives the
 * c02 bench run a storage-only repro (no BLE, no blobs needed):
 *
 *   1. dump the compile-time flash geometry + the `storage` flash area
 *      (offset/size — compare against partitions_0x0_default_16M.dtsi),
 *   2. open the storage area and exercise it with a settings
 *      save/load round-trip, logging every return code,
 *   3. print the result verdict next to the known failure signature.
 *
 * Enable with zmk/esp-port/bringup/c02-ble-repro.conf (BLE variant) or a
 * BLE-off conf with CONFIG_ZMK_TAB1A_DEBUG=y. Not for product builds.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/storage/flash_map.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define DUMP_DELAY_MS 5000

static bool roundtrip_read_ok;
static int32_t roundtrip_value = -1;

/* h_set receives every "tab1a_dbg/*" key during settings load; the
 * settings cache is not used here, so this also serves the read-back. */
static int dbg_settings_handle_set(const char *key, size_t len, settings_read_cb read_cb,
                                   void *cb_arg, void *param) {
    int err = read_cb(cb_arg, &roundtrip_value, sizeof(roundtrip_value));
    if (err < 0) {
        LOG_ERR("c02dbg: settings read_cb failed (%d)", err);
        return err;
    }

    roundtrip_read_ok = true;
    return 0;
}

static struct settings_handler dbg_handler = {
    .name = "tab1a_dbg",
    .h_set = dbg_settings_handle_set,
};

static void dump_flash_geometry(void) {
    /* NOTE: the ESP port's partition dtsi uses "zephyr,mapped-partition",
     * so FLASH_AREA_ID()/flash_area_open() are NOT generated here — dump
     * the compile-time geometry straight from DT (offset/size must match
     * what the runtime partition lookup resolves; compare against
     * partitions_0x0_default_16M.dtsi). */
    if (DT_NODE_EXISTS(DT_NODELABEL(storage_partition))) {
        LOG_INF("c02dbg: storage_partition offset=0x%zx size=%zu",
                (size_t)DT_REG_ADDR(DT_NODELABEL(storage_partition)),
                (size_t)DT_REG_SIZE(DT_NODELABEL(storage_partition)));
    } else {
        LOG_ERR("c02dbg: no storage_partition node in DT");
    }

    LOG_INF("c02dbg: flash0 reg: offset=0x%zx size=%zu",
            (size_t)DT_REG_ADDR(DT_CHOSEN(zephyr_flash)),
            (size_t)DT_REG_SIZE(DT_CHOSEN(zephyr_flash)));
}

static void storage_probe_work_fn(struct k_work *work) {
    LOG_INF("c02dbg: ---- storage/flash dump start ----");
    LOG_INF("c02dbg: CONFIG_ESPTOOLPY_FLASHSIZE=%s", STRINGIFY(CONFIG_ESPTOOLPY_FLASHSIZE));

    dump_flash_geometry();

    int err = settings_register(&dbg_handler);
    if (err && err != -EEXIST) {
        LOG_ERR("c02dbg: settings_register failed (%d)", err);
        return;
    }

    int32_t magic = 0x5ab1a042;
    err = settings_save_one("tab1a_dbg/value", &magic, sizeof(magic));
    LOG_INF("c02dbg: settings_save_one -> %d%s", err,
            err ? "  <== this is the c02 write failure path" : "");

    if (err == 0) {
        roundtrip_read_ok = false;
        roundtrip_value = -1;
        err = settings_load_subtree_direct("tab1a_dbg", dbg_settings_handle_set, NULL);
        if (err == 0 && roundtrip_read_ok && roundtrip_value == magic) {
            LOG_INF("c02dbg: settings round-trip OK — storage healthy");
        } else {
            LOG_ERR("c02dbg: round-trip FAILED (err %d, read_ok %d, value 0x%08x)", err,
                    roundtrip_read_ok, roundtrip_value);
        }
    }

    LOG_INF("c02dbg: ---- storage/flash dump end ----");
}

K_WORK_DELAYABLE_DEFINE(storage_probe_work, storage_probe_work_fn);

static int tab1a_debug_init(void) {
    k_work_schedule(&storage_probe_work, K_MSEC(DUMP_DELAY_MS));
    return 0;
}

SYS_INIT(tab1a_debug_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
