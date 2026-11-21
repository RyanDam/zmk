/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Hardware stats screen for the Coban Tab 1a (backlog a11).
 *
 * Renders 4 slots fed through the zmk/stats.h seam — today by the mock
 * feed, from a14 by the Studio RPC, with no UI change either way (the
 * screen touches the seam API and nothing else; that is the acceptance
 * grep). Metric ids/unit ids come from the seam so the enum tables and
 * the host payload cannot drift apart.
 *
 * pad10a layouts (2x1: two gauges, 2x2: four bars, hybrid: 1 big + 2)
 * are re-thought for the portrait panel and a 2x4 variant is added:
 *   2x1  two tall cells side by side   (slots 0-1)
 *   2x2  2x2 grid                      (slots 0-3)
 *   HYBR one big cell + three rows    (slots 0-3)
 *   2x4  four wide rows, big bars      (slots 0-3)
 *
 * Touch: tap a cell to cycle its metric id through all 16 metrics
 * (reaches the whole enum table on the glass); tap anywhere else in the
 * content area to cycle layouts. Layout + metric ids persist in
 * settings ("display/stats/config", pad10a eeprom cb_eeprom.h:51-53
 * field set). Stale readings (seam reports stale) grey out their bar.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>

#include <lvgl.h>

#include <zmk/display.h>
#include <zmk/display/screens.h>
#include <zmk/stats.h>

#include "styles.h"
#include "ui_helpers.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define PAGE_NAME "stats"

enum stats_layout {
    STATS_LAYOUT_2X1 = 0,
    STATS_LAYOUT_2X2,
    STATS_LAYOUT_HYBRID,
    STATS_LAYOUT_2X4,
    STATS_LAYOUT_COUNT
};

struct stats_cfg {
    uint8_t layout;
    uint8_t metric[ZMK_STATS_SLOT_COUNT];
};

static struct stats_cfg cfg = {
    .layout = STATS_LAYOUT_2X2,
    .metric = {ZMK_STATS_CPU_UTIL, ZMK_STATS_GPU_UTIL, ZMK_STATS_RAM, ZMK_STATS_CPU_TEMP},
};

struct cell {
    lv_obj_t *box;
    lv_obj_t *label;
    lv_obj_t *value;
    lv_obj_t *fill;
    lv_obj_t *max_label;
};

static struct cell cells[ZMK_STATS_SLOT_COUNT];
static lv_obj_t *screen_root;
static lv_obj_t *mock_label;

/* ------------------------------------------------------------------ */
/* Settings                                                            */
/* ------------------------------------------------------------------ */

#if IS_ENABLED(CONFIG_SETTINGS)
static void cfg_save(void) {
    int err = settings_save_one("display/stats/config", &cfg, sizeof(cfg));

    if (err) {
        LOG_WRN("Could not persist stats config (err %d) - live apply only", err);
    }
}
#endif

/* ------------------------------------------------------------------ */
/* Cells + layouts                                                     */
/* ------------------------------------------------------------------ */

static lv_obj_t *mk_cell(lv_obj_t *parent, int w, int h, bool big) {
    lv_obj_t *box = lv_obj_create(parent);

    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, w, h);
    lv_obj_set_style_pad_all(box, ZMK_GRID_UNIT, 0);

    int slot = 0;

    for (int i = 0; i < ZMK_STATS_SLOT_COUNT; i++) {
        if (cells[i].box == NULL) {
            slot = i;
            break;
        }
    }

    lv_obj_t *col = zmk_ui_flex(box, LV_FLEX_FLOW_COLUMN, ZMK_GRID_UNIT / 2);

    lv_obj_align(col, LV_ALIGN_TOP_MID, 0, 0);

    struct cell *c = &cells[slot];

    c->box = box;
    c->label = zmk_ui_label(col, ZMK_STYLE_TEXT_CAPTION);
    c->value = zmk_ui_label(col, big ? ZMK_STYLE_TEXT_TITLE : ZMK_STYLE_TEXT_TABULAR);
    c->max_label = zmk_ui_label(col, ZMK_STYLE_TEXT_CAPTION);

    /* bar: fixed track, % fill child */
    lv_obj_t *track = lv_obj_create(col);

    lv_obj_remove_style_all(track);
    lv_obj_set_size(track, w - 2 * ZMK_GRID_UNIT, big ? ZMK_GRID_UNIT * 2 : ZMK_GRID_UNIT);
    lv_obj_add_style(track, zmk_display_style(ZMK_STYLE_INACTIVE), 0);
    lv_obj_set_style_radius(track, LV_RADIUS_CIRCLE, 0);

    c->fill = lv_obj_create(track);
    lv_obj_remove_style_all(c->fill);
    lv_obj_set_size(c->fill, 0, lv_pct(100));
    lv_obj_align(c->fill, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_add_style(c->fill, zmk_display_style(ZMK_STYLE_ACCENT), 0);
    lv_obj_set_style_radius(c->fill, LV_RADIUS_CIRCLE, 0);

    return box;
}

static void cells_clear(void) {
    for (int i = 0; i < ZMK_STATS_SLOT_COUNT; i++) {
        memset(&cells[i], 0, sizeof(cells[i]));
    }
    if (screen_root != NULL) {
        lv_obj_clean(screen_root); /* content container; page dots are safe */
    }
}

static void layout_build(void) {
    cells_clear();

    lv_obj_t *title = zmk_ui_label(screen_root, ZMK_STYLE_TEXT_TITLE);

    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, ZMK_GRID_MARGIN + ZMK_GRID_UNIT);
    lv_label_set_text(title, "Stats");

    switch (cfg.layout) {
    case STATS_LAYOUT_2X1:
        for (int i = 0; i < 2; i++) {
            lv_obj_t *cell = mk_cell(screen_root, 144, 336, true);

            lv_obj_align(cell, LV_ALIGN_TOP_LEFT, ZMK_GRID_MARGIN + i * 160, 80);
        }
        break;
    case STATS_LAYOUT_HYBRID: {
        lv_obj_t *big = mk_cell(screen_root, 288, 168, true);

        lv_obj_align(big, LV_ALIGN_TOP_LEFT, ZMK_GRID_MARGIN, 80);
        for (int i = 0; i < 3; i++) {
            lv_obj_t *cell = mk_cell(screen_root, 288, 80, false);

            lv_obj_align(cell, LV_ALIGN_TOP_LEFT, ZMK_GRID_MARGIN, 80 + 176 + i * 88);
        }
        break;
    }
    case STATS_LAYOUT_2X4:
        for (int i = 0; i < 4; i++) {
            lv_obj_t *cell = mk_cell(screen_root, 288, 88, false);

            lv_obj_align(cell, LV_ALIGN_TOP_LEFT, ZMK_GRID_MARGIN, 80 + i * 96);
        }
        break;
    case STATS_LAYOUT_2X2:
    default:
        for (int i = 0; i < 4; i++) {
            lv_obj_t *cell = mk_cell(screen_root, 144, 168, false);

            lv_obj_align(cell, LV_ALIGN_TOP_LEFT, ZMK_GRID_MARGIN + (i % 2) * 160,
                         80 + (i / 2) * 184);
        }
        break;
    }

    /* footer naming the layout + the tap affordance, above the dots */
    static const char *const layout_names[] = {"2x1", "2x2", "hybrid", "2x4"};
    char buf[48];

    snprintk(buf, sizeof(buf), "%s - tap cell: metric, tap bg: layout",
             layout_names[MIN((unsigned int)cfg.layout, STATS_LAYOUT_COUNT - 1)]);
    lv_obj_t *foot = zmk_ui_label(screen_root, ZMK_STYLE_TEXT_CAPTION);

    lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, -ZMK_GRID_MARGIN * 3);
    lv_label_set_text(foot, buf);

    if (IS_ENABLED(CONFIG_ZMK_STATS_MOCK)) {
        mock_label = zmk_ui_label(screen_root, ZMK_STYLE_TEXT_CAPTION);
        lv_obj_align(mock_label, LV_ALIGN_TOP_RIGHT, -ZMK_GRID_MARGIN,
                     ZMK_GRID_MARGIN + ZMK_GRID_UNIT);
        lv_label_set_text(mock_label, "mock feed");
    }
}

/* ------------------------------------------------------------------ */
/* 1 Hz refresh                                                        */
/* ------------------------------------------------------------------ */

static void stats_update(void) {
    for (int i = 0; i < ZMK_STATS_SLOT_COUNT; i++) {
        struct cell *c = &cells[i];
        struct zmk_stats_reading r;

        if (c->box == NULL) {
            continue;
        }
        lv_label_set_text(c->label, zmk_stats_label(cfg.metric[i]));

        if (!zmk_stats_get(cfg.metric[i], &r)) {
            lv_label_set_text(c->value, "--");
            lv_label_set_text(c->max_label, "");
            lv_obj_set_width(c->fill, 0);
            continue;
        }

        char buf[24];

        snprintk(buf, sizeof(buf), "%u%s", r.value, zmk_stats_unit_suffix(r.unit_id));
        lv_label_set_text(c->value, buf);
        snprintk(buf, sizeof(buf), "/ %u%s", r.max_value, zmk_stats_unit_suffix(r.unit_id));
        lv_label_set_text(c->max_label, buf);

        int32_t pct = r.max_value > 0 ? ((int32_t)r.value * 100) / r.max_value : 0;

        lv_obj_set_width(c->fill, (int32_t)lv_obj_get_width(lv_obj_get_parent(c->fill)) *
                                      CLAMP(pct, 0, 100) / 100);

        /* stale grey-out: fill off the accent onto the inactive slot */
        if (r.stale) {
            lv_obj_remove_style(c->fill, zmk_display_style(ZMK_STYLE_ACCENT), 0);
            lv_obj_add_style(c->fill, zmk_display_style(ZMK_STYLE_INACTIVE), 0);
        } else {
            lv_obj_remove_style(c->fill, zmk_display_style(ZMK_STYLE_INACTIVE), 0);
            lv_obj_add_style(c->fill, zmk_display_style(ZMK_STYLE_ACCENT), 0);
        }
    }
}

static void stats_timer_cb(lv_timer_t *timer) {
    static uint32_t samples, cycle_sum;
    uint32_t start = k_cycle_get_32();

    ARG_UNUSED(timer);

    /* a11 acceptance: 1 Hz redraw cost vs the ~61 ms full frame (R5) */
    stats_update();
    cycle_sum += k_cycle_get_32() - start;
    if (++samples >= 600) {
        LOG_INF("stats update avg %u us over %u s",
                (unsigned int)k_cyc_to_us_floor32(cycle_sum / samples), samples);
        samples = cycle_sum = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Touch: cell -> cycle metric, background -> cycle layout             */
/* ------------------------------------------------------------------ */

ZMK_UI_TAP_STATE(stats_tap);

static void stats_tap_work_cb(struct k_work *work);
K_WORK_DEFINE(stats_tap_work, stats_tap_work_cb);

static void stats_tap_work_cb(struct k_work *work) {
    uint16_t x = stats_tap.x, y = stats_tap.y;

    ARG_UNUSED(work);

    for (int i = 0; i < ZMK_STATS_SLOT_COUNT; i++) {
        lv_area_t a;

        if (cells[i].box == NULL) {
            continue;
        }
        lv_obj_get_coords(cells[i].box, &a);
        if (x >= a.x1 && x <= a.x2 && y >= a.y1 && y <= a.y2) {
            cfg.metric[i] = (cfg.metric[i] % (ZMK_STATS_ID_COUNT - 1)) + 1; /* 1..16 */
            LOG_INF("stats slot %u -> metric %u (%s)", i, cfg.metric[i],
                    zmk_stats_label(cfg.metric[i]));
#if IS_ENABLED(CONFIG_SETTINGS)
            cfg_save();
#endif
            stats_update();
            return;
        }
    }

    cfg.layout = (cfg.layout + 1) % STATS_LAYOUT_COUNT;
    LOG_INF("stats layout -> %u", cfg.layout);
#if IS_ENABLED(CONFIG_SETTINGS)
    cfg_save();
#endif
    layout_build();
    stats_update();
}

static void stats_touch_cb(struct input_event *evt, void *user_data) {
    ARG_UNUSED(user_data);
    zmk_ui_tap_input(&stats_tap, evt);
}
INPUT_CALLBACK_DEFINE(NULL, stats_touch_cb, NULL);

/* ------------------------------------------------------------------ */
/* Page registration                                                   */
/* ------------------------------------------------------------------ */

static lv_obj_t *stats_page_create(lv_obj_t *parent) {
    screen_root = lv_obj_create(parent);
    lv_obj_remove_style_all(screen_root);
    lv_obj_set_size(screen_root, lv_pct(100), lv_pct(100));

    zmk_ui_tap_init(&stats_tap, PAGE_NAME, &stats_tap_work);

    layout_build();
    stats_update();
    lv_timer_create(stats_timer_cb, 1000, NULL);
    return parent;
}

ZMK_DISPLAY_DEFINE_SCREEN(stats, PAGE_NAME, stats_page_create, 30);

/* ------------------------------------------------------------------ */
/* Settings load                                                       */
/* ------------------------------------------------------------------ */

#if IS_ENABLED(CONFIG_SETTINGS)
static int stats_settings_handle_set(const char *key, size_t len, settings_read_cb read_cb,
                                     void *cb_arg) {
    struct stats_cfg loaded;
    int err;

    if (strcmp("config", key) != 0) {
        return 0;
    }
    if (len != sizeof(loaded)) {
        LOG_WRN("bad stats config settings size %zu", len);
        return -EINVAL;
    }

    err = read_cb(cb_arg, &loaded, sizeof(loaded));
    if (err <= 0) {
        LOG_ERR("Failed to read stats config from settings (err %d)", err);
        return err;
    }
    if (loaded.layout >= STATS_LAYOUT_COUNT) {
        LOG_WRN("stats config layout out of range - ignored");
        return -EINVAL;
    }
    for (int i = 0; i < ZMK_STATS_SLOT_COUNT; i++) {
        if (loaded.metric[i] >= ZMK_STATS_ID_COUNT) {
            loaded.metric[i] = ZMK_STATS_NONE;
        }
    }

    cfg = loaded;
    LOG_INF("stats config loaded from settings");
    return 0; /* page may not exist yet; first create uses cfg */
}

SETTINGS_STATIC_HANDLER_DEFINE(display_stats, "display/stats", NULL, stats_settings_handle_set,
                               NULL, NULL);
#endif /* CONFIG_SETTINGS */
