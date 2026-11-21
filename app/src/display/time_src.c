/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Time-source seam implementation (backlog a09). The simulation backend is
 * the successor of the old CLOCK_BASE_EPOCH_S hack in clock_screen.c: a
 * Kconfig reference epoch advanced by uptime, flagged unsynced so the UI
 * can show that the time is not real yet (h02's DS1302 arrival flips the
 * flag through zmk_time_source_set()/zmk_time_sync()).
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zmk/display/time_src.h>

LOG_MODULE_REGISTER(zmk_time_src, CONFIG_ZMK_LOG_LEVEL);

static zmk_time_source_fn external_source;

/* Simulation state: displayed epoch = sim_base + uptime. zmk_time_sync()
 * rebases instead of storing a second timestamp, so the advance keeps the
 * same monotonic clock the unsynced sim used. */
static int64_t sim_base = CONFIG_ZMK_DISPLAY_CLOCK_EPOCH_S;
static bool sim_synced;

/*
 * Convert days since 1970-01-01 to a civil date.
 * (Howard Hinnant's days_from_civil inverse — same algorithm the pre-a09
 * clock screen used, moved here so every screen shares it.)
 */
static void civil_from_days(int32_t z, uint16_t *year, uint8_t *month, uint8_t *day) {
    z += 719468;
    int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    uint32_t doe = (uint32_t)(z - (int64_t)era * 146097);
    uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    uint32_t y = yoe + (uint32_t)era * 400;
    uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint32_t mp = (5 * doy + 2) / 153;
    uint32_t d = doy - (153 * mp + 2) / 5 + 1;
    uint32_t m = (mp < 10) ? mp + 3 : mp - 9;

    *year = (uint16_t)(y + (m <= 2));
    *month = (uint8_t)m;
    *day = (uint8_t)d;
}

void zmk_time_source_set(zmk_time_source_fn fn) {
    external_source = fn;
    LOG_INF("time source: %s", fn ? "hardware backend" : "simulation");
}

int64_t zmk_time_now_epoch(bool *synced_out) {
    if (external_source != NULL) {
        bool synced = false;
        int64_t epoch = external_source(&synced);

        if (synced_out != NULL) {
            *synced_out = synced;
        }
        return epoch;
    }

    if (synced_out != NULL) {
        *synced_out = sim_synced;
    }
    return sim_base + k_uptime_get() / 1000;
}

int zmk_time_now(struct zmk_time *out) {
    bool synced;
    int64_t now = zmk_time_now_epoch(&synced);

    if (now < 0) {
        return -1;
    }

    int32_t days = (int32_t)(now / 86400);
    int32_t rem = (int32_t)(now % 86400);

    out->hour = (uint8_t)(rem / 3600);
    out->minute = (uint8_t)((rem % 3600) / 60);
    out->second = (uint8_t)(rem % 60);
    civil_from_days(days, &out->year, &out->month, &out->day);
    /* 1970-01-01 was a Thursday. */
    out->weekday = (uint8_t)(((days % 7) + 4) % 7);
    out->synced = synced;
    return 0;
}

void zmk_time_sync(int64_t epoch) {
    sim_base = epoch - k_uptime_get() / 1000;
    sim_synced = true;
    LOG_INF("time synced to %lld (sim backend re-based)", (long long)epoch);
}

void zmk_time_mark_unsynced(void) {
    sim_base = CONFIG_ZMK_DISPLAY_CLOCK_EPOCH_S;
    sim_synced = false;
}
