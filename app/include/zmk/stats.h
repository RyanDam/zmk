/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Coban Tab 1a stats feed seam (backlog a11).
 *
 * Pad 10A's stats screen was written to by the companion app through
 * HID (hid.c:71-93 set_stats_data). On tab1a that push arrives later as
 * the a14 Studio RPC, so screens read a slot table nobody outside this
 * API writes: today the writer is the mock feed, tomorrow the RPC
 * subsystem calls the exact same zmk_stats_update().
 *
 * The metric/unit ids and the {value, max_value, unit_id} payload are
 * ported value-for-value from pad10a screen_hardware_stats.h:29-63 —
 * they are the a14 wire shape, do not renumber.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define ZMK_STATS_ID_COUNT 17  /* == pad10a STATS_DATA_COUNT */
#define ZMK_STATS_SLOT_COUNT 4 /* metrics shown at once (pad10a eeprom: 4 ids) */

/** Metric ids, ported unchanged from pad10a (0 = none/empty slot). */
enum zmk_stats_id {
    ZMK_STATS_NONE = 0x00,
    ZMK_STATS_CPU_UTIL = 0x01,
    ZMK_STATS_CPU_TEMP = 0x02,
    ZMK_STATS_RAM = 0x03,
    ZMK_STATS_GPU_UTIL = 0x04,
    ZMK_STATS_GPU_TEMP = 0x05,
    ZMK_STATS_DISK_USAGE = 0x06,
    ZMK_STATS_DISK_TOTAL = 0x07,
    ZMK_STATS_NET_SENT = 0x08,
    ZMK_STATS_NET_RECV = 0x09,
    ZMK_STATS_SWAP_USAGE = 0x0A,
    ZMK_STATS_CPU_FREQ = 0x0B,
    ZMK_STATS_SOC_TEMP = 0x0C,
    ZMK_STATS_DISK_READ = 0x0D,
    ZMK_STATS_DISK_WRITE = 0x0E,
    ZMK_STATS_CPU_POWER = 0x0F,
    ZMK_STATS_GPU_POWER = 0x10,
};

/** Unit ids, ported unchanged. */
enum zmk_stats_unit {
    ZMK_STATS_UNIT_NONE = 0x00,
    ZMK_STATS_UNIT_PERCENT = 0x01,
    ZMK_STATS_UNIT_CELSIUS = 0x02,
    ZMK_STATS_UNIT_FAHRENHEIT = 0x03,
    ZMK_STATS_UNIT_MEGABYTES = 0x04,
    ZMK_STATS_UNIT_GIGABYTES = 0x05,
    ZMK_STATS_UNIT_TERABYTES = 0x06,
    ZMK_STATS_UNIT_MBPS = 0x07,
    ZMK_STATS_UNIT_MHZ = 0x08,
    ZMK_STATS_UNIT_WATTS = 0x09,
};

/** One reading (pad10a coban_stats_data_t + freshness). */
struct zmk_stats_reading {
    uint16_t value;
    uint16_t max_value;
    uint8_t unit_id;
    bool valid; /* ever updated                                          */
    bool stale; /* no update for CONFIG_ZMK_STATS_STALE_MS               */
};

/** The only write path. @p id must be < ZMK_STATS_ID_COUNT. */
void zmk_stats_update(uint8_t id, uint16_t value, uint16_t max_value, uint8_t unit_id);

/** Read one metric; returns false when never written. */
bool zmk_stats_get(uint8_t id, struct zmk_stats_reading *out);

/** Short uppercase label for a metric id (never NULL, "" for none). */
const char *zmk_stats_label(uint8_t id);

/** Unit suffix for a unit id ("" for none, "%", " C", " GB" …). */
const char *zmk_stats_unit_suffix(uint8_t unit_id);

/* ---------------------------------------------------------------- --
 * Display config (a14): layout + which metric fills which slot.
 * Persisted ("display/stats/config" blob, same shape the a11 screen
 * always used); writers are the screen's taps, the Studio RPC and a
 * settings load — all through zmk_stats_config_set(), which raises
 * zmk_stats_config_changed so the screen rebuilds.
 * ------------------------------------------------------------------ */

/** Screen layouts, ids == the persisted blob's layout byte. */
enum zmk_stats_layout {
    ZMK_STATS_LAYOUT_2X1 = 0,
    ZMK_STATS_LAYOUT_2X2,
    ZMK_STATS_LAYOUT_HYBRID,
    ZMK_STATS_LAYOUT_2X4,
    ZMK_STATS_LAYOUT_COUNT
};

struct zmk_stats_config {
    uint8_t layout; /* enum zmk_stats_layout                        */
    uint8_t metric_ids[ZMK_STATS_SLOT_COUNT]; /* enum zmk_stats_id  */
};

/** Live config; pointer stays valid (copy to mutate). */
const struct zmk_stats_config *zmk_stats_config_get(void);

/** Clamp + persist + raise zmk_stats_config_changed. Safe from any
 * thread; no-op (0, no event) when the config is unchanged. */
int zmk_stats_config_set(const struct zmk_stats_config *cfg);
