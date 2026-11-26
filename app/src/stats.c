/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Stats feed seam implementation: a 17-entry sample table behind one write
 * call. The mock feed (CONFIG_ZMK_STATS_MOCK) is just another client of it.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/spinlock.h>

#include <zmk/events/stats_config_changed.h>
#include <zmk/stats.h>

LOG_MODULE_REGISTER(zmk_stats, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/settings_blob.h>

struct sample {
    uint16_t value;
    uint16_t max_value;
    uint8_t unit_id;
    bool valid;
    int64_t updated_ms;
};

static struct sample table[ZMK_STATS_ID_COUNT];
static struct k_spinlock table_lock;

void zmk_stats_update(uint8_t id, uint16_t value, uint16_t max_value, uint8_t unit_id) {
    if (id == ZMK_STATS_NONE || id >= ZMK_STATS_ID_COUNT) {
        return;
    }

    k_spinlock_key_t key = k_spin_lock(&table_lock);

    table[id].value = value;
    table[id].max_value = max_value;
    table[id].unit_id = unit_id;
    table[id].valid = true;
    table[id].updated_ms = k_uptime_get();
    k_spin_unlock(&table_lock, key);
}

bool zmk_stats_get(uint8_t id, struct zmk_stats_reading *out) {
    if (id >= ZMK_STATS_ID_COUNT || out == NULL) {
        return false;
    }

    k_spinlock_key_t key = k_spin_lock(&table_lock);
    struct sample s = table[id];

    k_spin_unlock(&table_lock, key);

    out->value = s.value;
    out->max_value = s.max_value;
    out->unit_id = s.unit_id;
    out->valid = s.valid;
    out->stale = s.valid && (k_uptime_get() - s.updated_ms) > CONFIG_ZMK_STATS_STALE_MS;
    return s.valid;
}

static const char *const labels[ZMK_STATS_ID_COUNT] = {
    [ZMK_STATS_NONE] = "",           [ZMK_STATS_CPU_UTIL] = "CPU",
    [ZMK_STATS_CPU_TEMP] = "CPU T",  [ZMK_STATS_RAM] = "RAM",
    [ZMK_STATS_GPU_UTIL] = "GPU",    [ZMK_STATS_GPU_TEMP] = "GPU T",
    [ZMK_STATS_DISK_USAGE] = "DISK", [ZMK_STATS_DISK_TOTAL] = "DISK N",
    [ZMK_STATS_NET_SENT] = "TX",     [ZMK_STATS_NET_RECV] = "RX",
    [ZMK_STATS_SWAP_USAGE] = "SWAP", [ZMK_STATS_CPU_FREQ] = "CLK",
    [ZMK_STATS_SOC_TEMP] = "SOC T",  [ZMK_STATS_DISK_READ] = "R/S",
    [ZMK_STATS_DISK_WRITE] = "W/S",  [ZMK_STATS_CPU_POWER] = "CPU W",
    [ZMK_STATS_GPU_POWER] = "GPU W",
};

static const char *const units[] = {
    [ZMK_STATS_UNIT_NONE] = "",         [ZMK_STATS_UNIT_PERCENT] = "%",
    [ZMK_STATS_UNIT_CELSIUS] = " C",    [ZMK_STATS_UNIT_FAHRENHEIT] = " F",
    [ZMK_STATS_UNIT_MEGABYTES] = " MB", [ZMK_STATS_UNIT_GIGABYTES] = " GB",
    [ZMK_STATS_UNIT_TERABYTES] = " TB", [ZMK_STATS_UNIT_MBPS] = " Mb/s",
    [ZMK_STATS_UNIT_MHZ] = " MHz",      [ZMK_STATS_UNIT_WATTS] = " W",
};

const char *zmk_stats_label(uint8_t id) {
    if (id >= ZMK_STATS_ID_COUNT || labels[id] == NULL) {
        return "?";
    }
    return labels[id];
}

const char *zmk_stats_unit_suffix(uint8_t unit_id) {
    if (unit_id >= ARRAY_SIZE(units) || units[unit_id] == NULL) {
        return "";
    }
    return units[unit_id];
}

/* Display config: layout + slot metrics, persisted */

static struct zmk_stats_config config = {
    .layout = ZMK_STATS_LAYOUT_2X2,
    .metric_ids = {ZMK_STATS_CPU_UTIL, ZMK_STATS_GPU_UTIL, ZMK_STATS_RAM, ZMK_STATS_CPU_TEMP},
};

static void config_clamp(struct zmk_stats_config *c) {
    if (c->layout >= ZMK_STATS_LAYOUT_COUNT) {
        c->layout = ZMK_STATS_LAYOUT_2X2;
    }
    for (int i = 0; i < ZMK_STATS_SLOT_COUNT; i++) {
        if (c->metric_ids[i] >= ZMK_STATS_ID_COUNT) {
            c->metric_ids[i] = ZMK_STATS_NONE;
        }
    }
}

const struct zmk_stats_config *zmk_stats_config_get(void) { return &config; }

int zmk_stats_config_set(const struct zmk_stats_config *cfg) {
    if (cfg == NULL) {
        return -EINVAL;
    }

    struct zmk_stats_config next = *cfg;

    config_clamp(&next);

    bool layout_changed = next.layout != config.layout;

    if (memcmp(&next, &config, sizeof(next)) == 0) {
        return 0; /* idempotent: no write, no event */
    }

    config = next;

#if IS_ENABLED(CONFIG_SETTINGS)
    zmk_settings_blob_save("display/stats/config", &config, sizeof(config), "stats config");
#endif
    LOG_INF("stats config: layout %u, metrics %u/%u/%u/%u", config.layout, config.metric_ids[0],
            config.metric_ids[1], config.metric_ids[2], config.metric_ids[3]);

    raise_zmk_stats_config_changed((struct zmk_stats_config_changed){
        .layout_changed = layout_changed});
    return 0;
}

#if IS_ENABLED(CONFIG_SETTINGS)
static int stats_settings_handle_set(const char *key, size_t len, settings_read_cb read_cb,
                                     void *cb_arg) {
    struct zmk_stats_config loaded;
    int err = zmk_settings_blob_read(key, len, read_cb, cb_arg, "config", &loaded, sizeof(loaded),
                                     "stats config");

    if (err <= 0) {
        return err;
    }

    config_clamp(&loaded);
    config = loaded; /* load precedes any page build */
    LOG_INF("stats config loaded from settings");
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(display_stats, "display/stats", NULL, stats_settings_handle_set,
                               NULL, NULL);
#endif /* CONFIG_SETTINGS */

/* Mock feed (demo/dev) */

#if IS_ENABLED(CONFIG_ZMK_STATS_MOCK)

/* quarter-sine table, 0..1000, integer (16 steps, mirrored) */
static const uint16_t qsine[17] = {0,   195, 383, 556, 707, 831, 924, 981, 1000,
                                   981, 924, 831, 707, 556, 383, 195, 0};

struct mock_def {
    uint8_t id;
    uint16_t max; /* bar max_value; value swings between 0.2..1.0 of it */
    uint8_t unit;
    uint8_t speed; /* table steps advanced per second */
};

static const struct mock_def mocks[] = {
    {ZMK_STATS_CPU_UTIL, 100, ZMK_STATS_UNIT_PERCENT, 7},
    {ZMK_STATS_CPU_TEMP, 95, ZMK_STATS_UNIT_CELSIUS, 3},
    {ZMK_STATS_RAM, 32, ZMK_STATS_UNIT_GIGABYTES, 2},
    {ZMK_STATS_GPU_UTIL, 100, ZMK_STATS_UNIT_PERCENT, 11},
    {ZMK_STATS_GPU_TEMP, 90, ZMK_STATS_UNIT_CELSIUS, 4},
    {ZMK_STATS_DISK_USAGE, 2, ZMK_STATS_UNIT_TERABYTES, 1},
    {ZMK_STATS_DISK_TOTAL, 4, ZMK_STATS_UNIT_TERABYTES, 1},
    {ZMK_STATS_NET_SENT, 940, ZMK_STATS_UNIT_MBPS, 13},
    {ZMK_STATS_NET_RECV, 940, ZMK_STATS_UNIT_MBPS, 17},
    {ZMK_STATS_SWAP_USAGE, 8, ZMK_STATS_UNIT_GIGABYTES, 2},
    {ZMK_STATS_CPU_FREQ, 5600, ZMK_STATS_UNIT_MHZ, 5},
    {ZMK_STATS_SOC_TEMP, 85, ZMK_STATS_UNIT_CELSIUS, 6},
    {ZMK_STATS_DISK_READ, 7000, ZMK_STATS_UNIT_MEGABYTES, 19},
    {ZMK_STATS_DISK_WRITE, 5200, ZMK_STATS_UNIT_MEGABYTES, 23},
    {ZMK_STATS_CPU_POWER, 120, ZMK_STATS_UNIT_WATTS, 8},
    {ZMK_STATS_GPU_POWER, 350, ZMK_STATS_UNIT_WATTS, 9},
};

static void mock_tick(struct k_work *work);
K_WORK_DEFINE(mock_work, mock_tick);

static void mock_timer_cb(struct k_timer *timer) {
    ARG_UNUSED(timer);
    k_work_submit(&mock_work);
}
K_TIMER_DEFINE(mock_timer, mock_timer_cb, NULL);

static void mock_tick(struct k_work *work) {
    static uint32_t walk; /* free-running; per-metric speed/phase decorrelate */

    ARG_UNUSED(work);
    walk += 11;

    for (size_t i = 0; i < ARRAY_SIZE(mocks); i++) {
        const struct mock_def *m = &mocks[i];
        uint32_t pos = ((walk * m->speed) / 7 + i * 5) % 32;
        uint16_t wave = pos < 16 ? qsine[pos] : qsine[32 - pos]; /* 0..1000 */

        /* swing between 20 % and 100 % of max — never a dead bar */
        uint32_t value = ((uint32_t)m->max * (200 + ((uint32_t)wave * 8) / 10)) / 1000;

        zmk_stats_update(m->id, (uint16_t)value, m->max, m->unit);
    }
}

static int stats_mock_enable(void) {
    k_timer_start(&mock_timer, K_MSEC(1000), K_MSEC(1000));
    LOG_INF("stats mock feed running at 1 Hz (%zu metrics)", ARRAY_SIZE(mocks));
    return 0;
}
SYS_INIT(stats_mock_enable, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#endif /* CONFIG_ZMK_STATS_MOCK */
