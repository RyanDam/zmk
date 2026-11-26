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
 *   HYBR one big cell + three rows     (slots 0-3)
 *   2x4  four wide rows, big bars      (slots 0-3)
 *
 * Touch: tap a cell to cycle its metric id through all 16 metrics
 * (reaches the whole enum table on the glass); tap anywhere else in the
 * content area to cycle layouts. Layout + metric ids persist in
 * settings ("display/stats/config", pad10a eeprom cb_eeprom.h:51-53
 * field set). Stale readings (seam reports stale) grey out their bar.
 *
 * The 1 Hz tick is page-gated and every label/bar mutation is
 * skip-if-unchanged: on this full-refresh panel an unchanged tick costs
 * zero invalidations, and an invalidation is a ~61 ms flush.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <lvgl.h>

#include <zmk/display.h>
#include <zmk/display/screens.h>
#include <zmk/events/stats_config_changed.h>
#include <zmk/stats.h>

#include "styles.h"
#include "ui_helpers.h"
#if IS_ENABLED(CONFIG_ZMK_DISPLAY_ANIM)
#include "anim.h"
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define PAGE_NAME "stats"

/* Layout ids + the slot config live in zmk/stats.h (a14 seam): taps,
 * the Studio RPC and settings all write through zmk_stats_config_set. */

struct cell {
    lv_obj_t *box;
    lv_obj_t *label;
    lv_obj_t *value;
    lv_obj_t *fill;
    lv_obj_t *max_label;
    /* last-drawn text per label + last bar percent/stale state, so a
     * tick that changes nothing invalidates nothing */
    char label_cache[16];
    char value_cache[24];
    char max_cache[24];
    int32_t pct_cache;
    bool stale_cache;
};

static struct cell cells[ZMK_STATS_SLOT_COUNT];
static lv_obj_t *screen_root;
static lv_obj_t *mock_label;

/* ------------------------------------------------------------------ */
/* Cells + layouts                                                     */
/* ------------------------------------------------------------------ */

static lv_obj_t *mk_cell(lv_obj_t *parent, int slot, int w, int h, bool big) {
    lv_obj_t *box = lv_obj_create(parent);

    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, w, h);
    lv_obj_set_style_pad_all(box, ZMK_GRID_UNIT, 0);

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
    c->pct_cache = 0;
    c->stale_cache = false;

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

    const struct zmk_stats_config *cfg = zmk_stats_config_get();

    lv_obj_t *title = zmk_ui_label(screen_root, ZMK_STYLE_TEXT_TITLE);

    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, ZMK_GRID_MARGIN + ZMK_GRID_UNIT);
    lv_label_set_text(title, "Stats");

    switch (cfg->layout) {
    case ZMK_STATS_LAYOUT_2X1:
        for (int i = 0; i < 2; i++) {
            lv_obj_t *cell = mk_cell(screen_root, i, 216, 160, true);

            lv_obj_align(cell, LV_ALIGN_TOP_LEFT, ZMK_GRID_MARGIN + i * 232, 88);
        }
        break;
    case ZMK_STATS_LAYOUT_HYBRID: {
        lv_obj_t *big = mk_cell(screen_root, 0, 288, 160, true);

        lv_obj_align(big, LV_ALIGN_TOP_LEFT, ZMK_GRID_MARGIN, 88);
        for (int i = 0; i < 3; i++) {
            lv_obj_t *cell = mk_cell(screen_root, i + 1, 144, 52, false);

            /* right-hand column, flush to the safe margin */
            lv_obj_align(cell, LV_ALIGN_TOP_LEFT,
                         ZMK_GRID_SCREEN_W - ZMK_GRID_MARGIN - 144, 88 + i * 56);
        }
        break;
    }
    case ZMK_STATS_LAYOUT_2X4:
        /* Four across: at 320 px tall a column of four cannot give any cell
         * its text stack, but 104-px columns can. */
        for (int i = 0; i < 4; i++) {
            lv_obj_t *cell = mk_cell(screen_root, i, 104, 160, false);

            lv_obj_align(cell, LV_ALIGN_TOP_LEFT, ZMK_GRID_MARGIN + i * 112, 88);
        }
        break;
    case ZMK_STATS_LAYOUT_2X2:
    default:
        for (int i = 0; i < 4; i++) {
            lv_obj_t *cell = mk_cell(screen_root, i, 216, 76, false);

            lv_obj_align(cell, LV_ALIGN_TOP_LEFT, ZMK_GRID_MARGIN + (i % 2) * 232,
                         88 + (i / 2) * 84);
        }
        break;
    }

    /* footer naming the layout + the tap affordance, above the dots */
    static const char *const layout_names[] = {"2x1", "2x2", "hybrid", "2x4"};
    char buf[48];

    snprintk(buf, sizeof(buf), "%s - tap cell: metric, tap bg: layout",
             layout_names[MIN((unsigned int)cfg->layout, ZMK_STATS_LAYOUT_COUNT - 1)]);
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
/* 1 Hz refresh (page-gated, skip-if-unchanged)                        */
/* ------------------------------------------------------------------ */

static void stats_update(void) {
    const struct zmk_stats_config *cfg = zmk_stats_config_get();

    for (int i = 0; i < ZMK_STATS_SLOT_COUNT; i++) {
        struct cell *c = &cells[i];
        struct zmk_stats_reading r;

        if (c->box == NULL) {
            continue;
        }
        zmk_ui_set_text(c->label_cache, sizeof(c->label_cache), c->label,
                        zmk_stats_label(cfg->metric_ids[i]));

        if (!zmk_stats_get(cfg->metric_ids[i], &r)) {
            zmk_ui_set_text(c->value_cache, sizeof(c->value_cache), c->value, "--");
            zmk_ui_set_text(c->max_cache, sizeof(c->max_cache), c->max_label, "");
            if (c->pct_cache != 0) {
                lv_obj_set_width(c->fill, 0);
                c->pct_cache = 0;
            }
            continue;
        }

        char buf[24];

        snprintk(buf, sizeof(buf), "%u%s", r.value, zmk_stats_unit_suffix(r.unit_id));
        zmk_ui_set_text(c->value_cache, sizeof(c->value_cache), c->value, buf);
        snprintk(buf, sizeof(buf), "/ %u%s", r.max_value, zmk_stats_unit_suffix(r.unit_id));
        zmk_ui_set_text(c->max_cache, sizeof(c->max_cache), c->max_label, buf);

        int32_t pct = r.max_value > 0 ? ((int32_t)r.value * 100) / r.max_value : 0;

        pct = CLAMP(pct, 0, 100);
        if (pct != c->pct_cache) {
            lv_obj_set_width(c->fill,
                             (int32_t)lv_obj_get_width(lv_obj_get_parent(c->fill)) * pct / 100);
            c->pct_cache = pct;
        }

        /* stale grey-out: fill off the accent onto the inactive slot,
         * restyled only when the freshness flips */
        if (r.stale != c->stale_cache) {
            zmk_ui_style_toggle(c->fill, zmk_display_style(ZMK_STYLE_ACCENT), !r.stale);
            zmk_ui_style_toggle(c->fill, zmk_display_style(ZMK_STYLE_INACTIVE), r.stale);
            c->stale_cache = r.stale;
        }
    }
}

static void stats_tick(void) {
    static uint32_t samples, cycle_sum;
    uint32_t start = k_cycle_get_32();

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

/* Forward declaration: K_WORK_DEFINE below has external linkage (Zephyr
 * macro), and the tap state needs the symbol before its definition. */
struct k_work stats_tap_work;

/* Page name and work are compile-time initializers: the input callback is
 * global (a boot-time phantom touch fires before any page exists), so a
 * tap state must never depend on a runtime zmk_ui_tap_init() ordering.
 */
ZMK_UI_TAP_STATE(stats_tap) = { .page_name = PAGE_NAME, .work = &stats_tap_work };

ZMK_UI_WORK_DEFINE(stats_tap_work, stats_tap_work_cb) {
    /* The tap detector stores RAW panel coordinates; the cell rects are
     * logical. Convert before the hit test (ui_helpers.h). */
    int32_t x = stats_tap.x, y = stats_tap.y;

    ARG_UNUSED(work);

    zmk_ui_touch_to_logical(&x, &y);

    struct zmk_stats_config cfg = *zmk_stats_config_get();

    for (int i = 0; i < ZMK_STATS_SLOT_COUNT; i++) {
        lv_area_t a;

        if (cells[i].box == NULL) {
            continue;
        }
        lv_obj_get_coords(cells[i].box, &a);
        if (x >= a.x1 && x <= a.x2 && y >= a.y1 && y <= a.y2) {
            cfg.metric_ids[i] = (cfg.metric_ids[i] % (ZMK_STATS_ID_COUNT - 1)) + 1; /* 1..16 */
            LOG_INF("stats slot %u -> metric %u (%s)", i, cfg.metric_ids[i],
                    zmk_stats_label(cfg.metric_ids[i]));
            /* Persist + raise zmk_stats_config_changed; stats_cfg_work
             * below does the redraw (same path as an RPC/host change). */
            zmk_stats_config_set(&cfg);
            return;
        }
    }

    cfg.layout = (cfg.layout + 1) % ZMK_STATS_LAYOUT_COUNT;
    LOG_INF("stats layout -> %u", cfg.layout);
    zmk_stats_config_set(&cfg);
}

/* Config changed from anywhere (this screen's taps, the Studio RPC, a
 * settings load): rebuild when the layout moved, relabel otherwise.
 * Event cb only latches + posts; the work runs on the display queue. */
static void stats_cfg_rebuild(void);
static bool stats_cfg_layout_dirty;

ZMK_UI_WORK_DEFINE(stats_cfg_work, stats_cfg_work_cb) {
    bool layout_changed = stats_cfg_layout_dirty;

    ARG_UNUSED(work);
    stats_cfg_layout_dirty = false;

    if (screen_root == NULL || !lv_obj_is_valid(screen_root)) {
        return; /* page not built (yet): first create picks the config up */
    }

    if (layout_changed) {
        stats_cfg_rebuild();
    } else {
        stats_update();
    }
}

static int stats_cfg_changed_event_cb(const zmk_event_t *eh) {
    const struct zmk_stats_config_changed *ev = as_zmk_stats_config_changed(eh);

    if (ev) {
        stats_cfg_layout_dirty = ev->layout_changed;
    }
    zmk_ui_post(&stats_cfg_work);
    return 0;
}

ZMK_LISTENER(stats_cfg, stats_cfg_changed_event_cb);
ZMK_SUBSCRIPTION(stats_cfg, zmk_stats_config_changed);

static void stats_cfg_rebuild(void) {
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

#if IS_ENABLED(CONFIG_ZMK_DISPLAY_ANIM)
    /* a12 owner pass: animated background on every page, not just the
     * clock — under the widgets, same setting, lifecycle in anim.c. */
    zmk_anim_bg_attach(screen_root, PAGE_NAME);
#endif

    layout_build();
    stats_update();
    zmk_ui_page_timer(PAGE_NAME, 1000, stats_tick);
    return parent;
}

ZMK_DISPLAY_DEFINE_SCREEN(stats, PAGE_NAME, stats_page_create, 30);
