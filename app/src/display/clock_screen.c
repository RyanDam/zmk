/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Clock and calendar screen: the clock and calendar widgets in one of three
 * layouts — CLOCK_ONLY (144 px HH:MM centred, date caption above),
 * CALENDAR_ONLY (header + month grid), FUSION (stacked 72 px clock left, grid
 * right). Text is fixed HH:MM (24 h) and "DoW DD/MM/YYYY", shown
 * unconditionally; the time always comes from zmk/display/time_src.h.
 *
 * Layout plus both widgets' style ids is one config at settings
 * "display/clock/config", seeded from ZMK_DISPLAY_CLOCK_* and reached through
 * <zmk/display/clock_screen.h>, so an RPC-set style survives a reboot. With the
 * screen manager, a tap (not a swipe) cycles the layout for bench work. Every
 * color/font/size comes from the style registry.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>

#if !IS_ENABLED(CONFIG_ZMK_DISPLAY_DESIGN_SYSTEM)
#include <zephyr/random/random.h>
#endif

#include <lvgl.h>

#include <zmk/display.h>
#include <zmk/display/clock_screen.h>
#include <zmk/display/status_screen.h>
#include <zmk/display/time_src.h>

#if IS_ENABLED(CONFIG_ZMK_DISPLAY_DESIGN_SYSTEM)
#include "calendar_widget.h"
#include "clock_widget.h"
#include "styles.h"
#include "theme.h"
#include "ui_helpers.h"
#if IS_ENABLED(CONFIG_ZMK_DISPLAY_ANIM)
#include "anim.h"
#endif
#endif

#if IS_ENABLED(CONFIG_ZMK_DISPLAY_SCREEN_MGR)
#include <zmk/display/screens.h>
#endif

LOG_MODULE_REGISTER(display_clock, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/settings_blob.h>

#if IS_ENABLED(CONFIG_ZMK_DISPLAY_DESIGN_SYSTEM)

/* Screen config (layout + widget styles), persisted as one blob */

static const char *const weekday_full[] = {"Sunday",   "Monday", "Tuesday", "Wednesday",
                                           "Thursday", "Friday", "Saturday"};

static struct zmk_display_clock_config cfg = {
    .layout = CONFIG_ZMK_DISPLAY_CLOCK_LAYOUT,
    .clock_style = CONFIG_ZMK_DISPLAY_CLOCK_STYLE,
    .calendar_style = CONFIG_ZMK_DISPLAY_CLOCK_CALENDAR_STYLE,
};

/* The Kconfig ranges (0..2 / 0..2 / 0..0) must track these enums. */
BUILD_ASSERT(CONFIG_ZMK_DISPLAY_CLOCK_LAYOUT < ZMK_DISPLAY_CLOCK_LAYOUT_COUNT);
BUILD_ASSERT(CONFIG_ZMK_DISPLAY_CLOCK_STYLE < ZMK_CLOCK_STYLE_COUNT);
BUILD_ASSERT(CONFIG_ZMK_DISPLAY_CLOCK_CALENDAR_STYLE < ZMK_CALENDAR_STYLE_COUNT);
BUILD_ASSERT(sizeof(struct zmk_display_clock_config) == 3);

#if IS_ENABLED(CONFIG_SETTINGS)
#define CLOCK_SETTINGS_SUBTREE "display/clock"
#define CLOCK_SETTINGS_KEY "config"
#endif

/* Live objects of the current layout, NULL where the layout has none so the
 * updater stays layout-agnostic. Widgets are heap modules freed with the page. */
static lv_obj_t *clock_root;
static zmk_clock_widget_t *clock_w;
static zmk_calendar_widget_t *cal_w;
static lv_obj_t *l_date;

/* Clear these whenever labels are rebuilt: a stale cache suppresses the write. */
static char date_cache[32];

static void clock_build_layout(void);
static void clock_update_text(void);

/* Geometry, 8 px grid. One calendar lattice for both layouts: square 30 px
 * cells, vertical pitch 37 (30 + CAL_GAP 7). Height is header 35 + weekday 24
 * + 6*30 + 7*7 = 288, exactly the content band y 16..304, clear of the page
 * dots at 308. The full-page view spreads each row SPACE_BETWEEN over the
 * 448 px content width; the fusion view gives the grid 288 px flush right and
 * centres the stacked clock in the 160 px left of it (glyphs need ~90 px). */

#define CAL_CELL 40      /* square day-cell side (barlow 24 line height 26)   */
#define CAL_GAP 4        /* packed gap floor (band stretch raises it)         */
#define CAL_CELL_W 62    /* calendar-only wide cell: 7*62 + 6*CAL_GAP = 458,
                          * + the widget's 2x8 px pad_all = 474 of 480        */
#define CAL_FUSION_W 288 /* fusion grid width (column pitch ~41 px)           */
#define CAL_H (ZMK_GRID_SCREEN_H - 2 * ZMK_GRID_MARGIN)        /* content band  */
#define CAL_HEADER_W (ZMK_GRID_SCREEN_W - 2 * ZMK_GRID_MARGIN) /* 448    */

static void add_clock(lv_obj_t *parent, enum zmk_clock_size size, lv_align_t align, int x, int y) {
    clock_w = zmk_clock_widget_create(parent, size, cfg.clock_style);
    if (clock_w != NULL) {
        lv_obj_align(zmk_clock_widget_obj(clock_w), align, x, y);
    }
}

static void add_calendar(bool header, uint16_t header_w, uint16_t grid_w, uint16_t cell_w,
                         lv_align_t align, int x, int y) {
    struct zmk_calendar_opts opts = {
        .style = cfg.calendar_style,
        .header = header, /* false: grid only (fusion: the clock carries
                           * the time context, owner pass) */
        .header_w = header_w,
        .grid_w = grid_w,
        /* 0 = no stretch: rows stay CAL_GAP apart at natural height. */
        .band_h = 0,
        .cell = CAL_CELL,
        .cell_w = cell_w, /* 0 = square CAL_CELL (fusion lattice) */
        .col_gap = CAL_GAP,
        .row_gap = CAL_GAP,
    };

    cal_w = zmk_calendar_widget_create(clock_root, &opts);
    if (cal_w != NULL) {
        lv_obj_align(zmk_calendar_widget_obj(cal_w), align, x, y);
    }
}

/* Layout 0: CLOCK ONLY */

/* The date caption anchors 48 px above the 144 px hero box rather than to the
 * page top, where it sat at the screen edge. */
#define CLOCK_DATE_ABOVE (ZMK_GRID_UNIT * 6)

static void build_clock_only(void) {
    /* Hero dead centre; the date hangs above it on the same axis. */
    add_clock(clock_root, ZMK_CLOCK_SIZE_HUGE, LV_ALIGN_CENTER, 0, 0);

    l_date = zmk_ui_label(clock_root, ZMK_STYLE_TEXT_SECONDARY);

    /* Fixed full-width centred box: lv_obj_align_to runs against the label's
     * current width, and an auto-sized label is still empty (0) here, so it
     * would grow rightwards instead of staying centred. */
    lv_obj_set_width(l_date, lv_pct(100));
    lv_obj_set_style_text_align(l_date, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *hero = zmk_clock_widget_obj(clock_w);

    if (hero != NULL) {
        lv_obj_align_to(l_date, hero, LV_ALIGN_TOP_MID, 0, -CLOCK_DATE_ABOVE);
    } else {
        lv_obj_align(l_date, LV_ALIGN_TOP_MID, 0, ZMK_GRID_MARGIN);
    }
}

/* Layout 1: CALENDAR ONLY */

static void build_calendar_only(void) {
    /* grid_w 0 -> columns keep exactly CAL_GAP air; header_w 0 -> the header is
     * sized to the packed lattice so year/month align with the outer columns. */
    add_calendar(true, 0, 0, CAL_CELL_W, LV_ALIGN_CENTER, 0, 0);
}

/* Layout 2: FUSION */

static void build_fusion(void) {
    /* Centre the stacked clock in the space left of the grid: equal air to the
     * margin and to the calendar for any digit widths. One placement mechanism
     * only — a flex child that is also align-moved leans right. */
    lv_obj_t *zone = lv_obj_create(clock_root);

    lv_obj_remove_style_all(zone);
    lv_obj_set_size(zone, CAL_HEADER_W - CAL_FUSION_W, CAL_H);
    lv_obj_align(zone, LV_ALIGN_TOP_LEFT, ZMK_GRID_MARGIN, ZMK_GRID_MARGIN);

    add_clock(zone, ZMK_CLOCK_SIZE_STACKED, LV_ALIGN_CENTER, 0, 0);
    add_calendar(false, CAL_FUSION_W, CAL_FUSION_W, 0, LV_ALIGN_RIGHT_MID, -ZMK_GRID_MARGIN, 0);
}

static void clock_build_layout(void) {
    /* The page container belongs to the screen manager (it owns the dots). */
    lv_obj_clean(clock_root);
    clock_w = NULL;
    cal_w = NULL;
    l_date = NULL;
    date_cache[0] = '\0';

#if IS_ENABLED(CONFIG_ZMK_DISPLAY_ANIM)
    /* Background first so it paints underneath, and not clickable; one shared
     * player across all pages (anim.c). */
    zmk_anim_bg_attach(clock_root, "clock");
#endif

    switch (cfg.layout) {
    case ZMK_DISPLAY_CLOCK_LAYOUT_CALENDAR_ONLY:
        build_calendar_only();
        break;
    case ZMK_DISPLAY_CLOCK_LAYOUT_FUSION:
        build_fusion();
        break;
    case ZMK_DISPLAY_CLOCK_LAYOUT_CLOCK_ONLY:
    default:
        build_clock_only();
        break;
    }

    /* Nothing below the content band but the manager's page dots. */

    /* Fresh labels: seed everything from the live time now. */
    clock_update_text();
}

/* One-second update */

static void clock_update_text(void) {
    struct zmk_time t;
    char buf[32];

    if (clock_root == NULL || zmk_time_now(&t) != 0) {
        return;
    }

    if (l_date != NULL) {
        snprintk(buf, sizeof(buf), "%s %02u/%02u/%04u", weekday_full[t.weekday], t.day, t.month,
                 t.year);
        zmk_ui_set_text(date_cache, sizeof(date_cache), l_date, buf);
    }

    zmk_clock_widget_update(clock_w, &t);
    zmk_calendar_widget_update(cal_w, &t);
}

static void clock_tick(void) {
    static uint32_t samples, cycle_sum;
    uint32_t start = k_cycle_get_32();

    /* Label updates must not starve touch; the ~61 ms full repaint is fixed. */
    clock_update_text();
    cycle_sum += k_cycle_get_32() - start;
    if (++samples >= 600) {
        LOG_INF("clock update avg %u us over %u s",
                (unsigned int)k_cyc_to_us_floor32(cycle_sum / samples), samples);
        samples = cycle_sum = 0;
    }
}

/* Config changes: settings save, rebuild, tap cycle, RPC seam */

ZMK_UI_WORK_DEFINE(clock_rebuild_work, clock_rebuild_work_cb) {
    ARG_UNUSED(work);

    if (clock_root == NULL) {
        return; /* page not built yet; the next create() uses cfg as-is */
    }
    clock_build_layout();
}

static void clock_schedule_rebuild(void) { zmk_ui_post(&clock_rebuild_work); }

#if IS_ENABLED(CONFIG_SETTINGS)
static void clock_settings_save(void) {
    zmk_settings_blob_save(CLOCK_SETTINGS_SUBTREE "/" CLOCK_SETTINGS_KEY, &cfg, sizeof(cfg),
                           "clock config");
}
#endif

/* Bench: a tap on the page cycles the layout (clock → calendar → fusion),
 * logged on the console. */
#define CLOCK_DEV_FIELDS 1
static uint8_t dev_field;

ZMK_UI_WORK_DEFINE(clock_cycle_field_work, clock_cycle_field_work_cb) {
    ARG_UNUSED(work);

    switch (dev_field) {
    case 0:
        cfg.layout = (cfg.layout + 1) % ZMK_DISPLAY_CLOCK_LAYOUT_COUNT;
        break;
    }

    LOG_INF("clock cfg: layout %u clock %u calendar %u (field %u cycled)", cfg.layout,
            cfg.clock_style, cfg.calendar_style, dev_field);
    dev_field = (dev_field + 1) % CLOCK_DEV_FIELDS;

#if IS_ENABLED(CONFIG_SETTINGS)
    clock_settings_save();
#endif
    clock_schedule_rebuild();
}

int zmk_display_clock_config_get(struct zmk_display_clock_config *out) {
    if (out == NULL) {
        return -EINVAL;
    }
    *out = cfg;
    return 0;
}

int zmk_display_clock_config_set(const struct zmk_display_clock_config *next) {
    if (next == NULL) {
        return -EINVAL;
    }
    if (next->layout >= ZMK_DISPLAY_CLOCK_LAYOUT_COUNT ||
        next->clock_style >= ZMK_CLOCK_STYLE_COUNT ||
        next->calendar_style >= ZMK_CALENDAR_STYLE_COUNT) {
        return -EINVAL;
    }
    if (memcmp(&cfg, next, sizeof(cfg)) == 0) {
        return 0;
    }

    cfg = *next;
    LOG_INF("clock cfg <- host: layout %u clock %u calendar %u", cfg.layout, cfg.clock_style,
            cfg.calendar_style);

#if IS_ENABLED(CONFIG_SETTINGS)
    clock_settings_save();
#endif
    clock_schedule_rebuild();
    return 0;
}

/* Page registration / standalone screen */

#if IS_ENABLED(CONFIG_ZMK_DISPLAY_SCREEN_MGR)
/* Compile-time bound (see stats_screen.c): the input callback can fire early. */
ZMK_UI_TAP_STATE(clock_tap) = {.page_name = "clock", .work = &clock_cycle_field_work};
#endif

static lv_obj_t *clock_attach(lv_obj_t *parent) {
    clock_root = lv_obj_create(parent);

    lv_obj_remove_style_all(clock_root);
    lv_obj_set_size(clock_root, lv_pct(100), lv_pct(100));

    clock_build_layout();
    zmk_ui_page_timer("clock", 1000, clock_tick);

    return parent;
}

#if IS_ENABLED(CONFIG_ZMK_DISPLAY_SCREEN_MGR)

static lv_obj_t *clock_page_create(lv_obj_t *parent) { return clock_attach(parent); }

ZMK_DISPLAY_DEFINE_SCREEN(clock, "clock", clock_page_create, 10);

/* Tap (not swipe) cycles one config field. The input callback runs in the
 * input context, so LVGL work goes to the display work queue; requiring
 * near-zero movement leaves the manager's swipe navigation alone. */
static void clock_touch_cb(struct input_event *evt, void *user_data) {
    ARG_UNUSED(user_data);
    zmk_ui_tap_input(&clock_tap, evt);
}
INPUT_CALLBACK_DEFINE(NULL, clock_touch_cb, NULL);

#else /* standalone status screen (no screen manager) */

lv_obj_t *zmk_display_status_screen(void) {
    lv_obj_t *screen = lv_obj_create(NULL);

    lv_obj_add_style(screen, zmk_display_style(ZMK_STYLE_SCREEN), 0);
    clock_attach(screen);

    LOG_INF("Clock status screen ready (simulated time, base epoch %lld)",
            (long long)CONFIG_ZMK_DISPLAY_CLOCK_EPOCH_S);
    return screen;
}

#endif /* CONFIG_ZMK_DISPLAY_SCREEN_MGR */

/* Settings */

#if IS_ENABLED(CONFIG_SETTINGS)
static uint8_t clock_cfg_sanitize(uint8_t value, unsigned int max, uint8_t fallback,
                                  const char *field) {
    if (value >= max) {
        LOG_WRN("clock config %s out of range (%u) - defaulting", field, value);
        return fallback;
    }
    return value;
}

static int clock_settings_handle_set(const char *key, size_t len, settings_read_cb read_cb,
                                     void *cb_arg) {
    struct zmk_display_clock_config loaded;
    int err = zmk_settings_blob_read(key, len, read_cb, cb_arg, CLOCK_SETTINGS_KEY, &loaded,
                                     sizeof(loaded), "clock config");

    if (err <= 0) {
        return err;
    }

    /* Per-field defaults: one unknown id must not discard the whole blob. */
    cfg.layout = clock_cfg_sanitize(loaded.layout, ZMK_DISPLAY_CLOCK_LAYOUT_COUNT,
                                    CONFIG_ZMK_DISPLAY_CLOCK_LAYOUT, "layout");
    cfg.clock_style = clock_cfg_sanitize(loaded.clock_style, ZMK_CLOCK_STYLE_COUNT,
                                         CONFIG_ZMK_DISPLAY_CLOCK_STYLE, "clock style");
    cfg.calendar_style =
        clock_cfg_sanitize(loaded.calendar_style, ZMK_CALENDAR_STYLE_COUNT,
                           CONFIG_ZMK_DISPLAY_CLOCK_CALENDAR_STYLE, "calendar style");
    clock_schedule_rebuild();
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(display_clock, CLOCK_SETTINGS_SUBTREE, NULL,
                               clock_settings_handle_set, NULL, NULL);
#endif /* CONFIG_SETTINGS */

#if IS_ENABLED(CONFIG_ZMK_DISPLAY_SCREEN_MGR)
/* Standalone keeps tap-to-cycle-theme; with the manager, tap drives config. */
#else
static void clock_theme_cycle_work_cb(struct k_work *work) {
    ARG_UNUSED(work);

    /* The controller reports a phantom touch at power-on, before the page. */
    if (clock_root == NULL) {
        return;
    }

    zmk_display_theme_cycle_preset();
}
K_WORK_DEFINE(clock_theme_cycle_work, clock_theme_cycle_work_cb);

static void clock_touch_cb(struct input_event *evt, void *user_data) {
    ARG_UNUSED(user_data);

    if (evt->type == INPUT_EV_KEY && evt->code == INPUT_BTN_TOUCH && evt->value == 1) {
        k_work_submit_to_queue(zmk_display_work_q(), &clock_theme_cycle_work);
    }
}
INPUT_CALLBACK_DEFINE(NULL, clock_touch_cb, NULL);
#endif

#else /* !CONFIG_ZMK_DISPLAY_DESIGN_SYSTEM — original hardcoded demo styling */

static const char *const weekday_names[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *const month_names[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
};

static lv_obj_t *clock_label;
static lv_obj_t *date_label;
static lv_obj_t *hint_label;

static void clock_update_text(void) {
    struct zmk_time t;
    char buf[32];

    if (zmk_time_now(&t) != 0) {
        return;
    }

    snprintk(buf, sizeof(buf), "%02u:%02u:%02u", t.hour, t.minute, t.second);
    lv_label_set_text(clock_label, buf);

    snprintk(buf, sizeof(buf), "%s, %s %u %u", weekday_names[t.weekday], month_names[t.month - 1],
             t.day, t.year);
    lv_label_set_text(date_label, buf);
}

static void clock_timer_cb(lv_timer_t *timer) {
    ARG_UNUSED(timer);
    clock_update_text();
}

/* Touch: the input callback's context forbids LVGL calls, so post the work. */
static void clock_random_color_work_cb(struct k_work *work) {
    ARG_UNUSED(work);

    /* Labels are built in this same queue; skip a phantom power-on touch. */
    if (clock_label == NULL || date_label == NULL) {
        return;
    }

    uint32_t rgb = sys_rand32_get();
    lv_color_t color =
        lv_color_make(96 + (rgb % 160), 96 + ((rgb >> 8) % 160), 96 + ((rgb >> 16) % 160));

    lv_obj_set_style_text_color(clock_label, color, 0);
    lv_obj_set_style_text_color(date_label, color, 0);
}
K_WORK_DEFINE(clock_random_color_work, clock_random_color_work_cb);

static void clock_touch_cb(struct input_event *evt, void *user_data) {
    ARG_UNUSED(user_data);

    if (evt->type == INPUT_EV_KEY && evt->code == INPUT_BTN_TOUCH && evt->value == 1) {
        k_work_submit_to_queue(zmk_display_work_q(), &clock_random_color_work);
    }
}
INPUT_CALLBACK_DEFINE(NULL, clock_touch_cb, NULL);

lv_obj_t *zmk_display_status_screen(void) {
    lv_obj_t *screen = lv_obj_create(NULL);

    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    clock_label = lv_label_create(screen);
    lv_obj_set_style_text_font(clock_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(clock_label, lv_color_white(), 0);
    lv_obj_align(clock_label, LV_ALIGN_CENTER, 0, -60);

    date_label = lv_label_create(screen);
    lv_obj_set_style_text_font(date_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(date_label, lv_color_white(), 0);
    lv_obj_align(date_label, LV_ALIGN_CENTER, 0, 10);

    hint_label = lv_label_create(screen);
    lv_obj_set_style_text_font(hint_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(hint_label, lv_color_make(128, 128, 128), 0);
    lv_obj_align(hint_label, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_label_set_text(hint_label, "tap the screen to change color");

    clock_update_text();
    lv_timer_create(clock_timer_cb, 1000, NULL);

    LOG_INF("Clock status screen ready (simulated time, base epoch %lld)",
            (long long)CONFIG_ZMK_DISPLAY_CLOCK_EPOCH_S);
    return screen;
}

#endif /* CONFIG_ZMK_DISPLAY_DESIGN_SYSTEM */
