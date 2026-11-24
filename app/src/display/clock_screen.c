/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Clock and calendar screen for display-centric boards (Coban Tab 1a).
 *
 * Wave-1 audit (2026-11-22): the a09 port's five time styles and the
 * pad10a format matrix (12/24 h, AM/PM, three date formats, date
 * visibility) were cut to a deliberate minimum. The page now composes
 * two widget modules in one of three layouts:
 *
 *   CLOCK_ONLY      huge HH:MM centred (144 px), small date caption
 *                   just above the digits (pomodoro-focus spacing)
 *   CALENDAR_ONLY   year top-left + month name top-right, full month grid
 *   FUSION          stacked clock (hour over minute, 72 px) left,
 *                   month grid right, symmetric split
 *
 * Time text is fixed HH:MM (24 h) and the date caption is fixed
 * "DoW DD/MM/YYYY" — the time itself still comes from the time-source
 * seam (zmk/display/time_src.h), never from uptime directly. The
 * wave-1 audit removed the unsynced affordance (dimmed digits +
 * "unsynced" caption): the screen shows the time unconditionally and
 * the synced flag stays inside the seam for h02 (DS1302) / a14 (host
 * set_time) to build the real sync UX on.
 *
 * Layout and the per-widget styles are one config (the widget style ids
 * live in clock_widget.h / calendar_widget.h): persisted in settings
 * ("display/clock/config"), seeded from the ZMK_DISPLAY_CLOCK_* Kconfig
 * defaults, readable/writable through the public seam in
 * <zmk/display/clock_screen.h> — which is what the a14 Studio RPC will
 * call, so a host-set style is also what survives the reboot.
 *
 * Dev/bench cycle: with the screen manager (a08) a tap ON the clock page
 * (not a swipe) cycles the layout — clock only → calendar → fusion; the
 * effect logs on the console, not on the glass. Swipe navigation and
 * the other pages are untouched.
 *
 * Styling rule (a07): every color/font/size comes from the style
 * registry; the legacy no-design-system path keeps the original
 * hardcoded demo.
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

/* --------------------------------------------------------------------- */
/* Screen config (layout + widget styles), persisted as one blob         */
/* --------------------------------------------------------------------- */

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

/* --------------------------------------------------------------------- */
/* Widgets + layout builders                                             */
/* --------------------------------------------------------------------- */

/* Live objects of the current layout; NULL where the layout has no such
 * element, so the one-second updater stays layout-agnostic. Widgets are
 * heap modules owned by the page subtree (they free themselves with it),
 * rebuilt fresh on every layout/style change. */
static lv_obj_t *clock_root;
static zmk_clock_widget_t *clock_w;
static zmk_calendar_widget_t *cal_w;
static lv_obj_t *l_date;

/* Label text caches. MUST be cleared whenever the labels are rebuilt —
 * a stale cache would suppress the first write to the fresh label. */
static char date_cache[32];

static void clock_build_layout(void);
static void clock_update_text(void);

/* Geometry (8 px grid, grid.md). One calendar lattice for both layouts:
 * SQUARE 30 px cells, vertical pitch 37 (30 + row_gap 7). Vertically the
 * block is header(35) + weekday(24) + 6*30 + 7*7 = 288 — exactly the
 * content band y 16..304, clear of the page dots (pip tops at 308).
 * Horizontally (owner photo pass 2026-11-22): the full-page view spreads
 * every row SPACE_BETWEEN across the whole 448 px content width — no
 * black side bands; the fusion view gives the grid 288 px flush right
 * (owner: the packed 252 felt cramped) and centres the stacked clock in
 * the 160 px space left of it — the glyphs need ~90 px, so the clock
 * keeps equal air to the margin and to the calendar while the lattice
 * breathes. Rows: only the 4..6 weeks the month occupies, gaps stretched
 * to fill the band. */

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
                           * the time context, owner pass 2026-11-22) */
        .header_w = header_w,
        .grid_w = grid_w,
        /* 0 = no band stretch: rows stay exactly CAL_GAP apart at the
         * widget's natural height (was CAL_H, which spread the page
         * slack into ~20 px row gaps regardless of CAL_GAP). */
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

/* ---------------- layout 0: CLOCK ONLY ------------------------------- */

/* Date caption follows the digit box, not the page — the pomodoro
 * focus caption's mechanism (pomodoro_screen.c CAP_ABOVE_COUNTDOWN,
 * anchored 32 px above the identical 144 px HERO_XL digit box) with
 * more air: 48 px (owner pass 2026-11-24: 32 still too tight, and
 * pinned at the page top it sat at the screen edge). */
#define CLOCK_DATE_ABOVE (ZMK_GRID_UNIT * 6)

static void build_clock_only(void) {
    /* Hero exactly at screen centre (owner pass 2026-11-24); the date
     * hangs above it on the same vertical axis. */
    add_clock(clock_root, ZMK_CLOCK_SIZE_HUGE, LV_ALIGN_CENTER, 0, 0);

    l_date = zmk_ui_label(clock_root, ZMK_STYLE_TEXT_SECONDARY);

    /* Page-wide box + centred text (pomodoro caption's trick):
     * lv_obj_align_to positions once, against the label's CURRENT
     * width — an auto-sized label is still empty here (width 0), so
     * it would start at the centre line and grow rightwards into a
     * right-aligned look when the date is written. A fixed full-width
     * box is symmetric around the centre line for any text. */
    lv_obj_set_width(l_date, lv_pct(100));
    lv_obj_set_style_text_align(l_date, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *hero = zmk_clock_widget_obj(clock_w);

    if (hero != NULL) {
        lv_obj_align_to(l_date, hero, LV_ALIGN_TOP_MID, 0, -CLOCK_DATE_ABOVE);
    } else {
        lv_obj_align(l_date, LV_ALIGN_TOP_MID, 0, ZMK_GRID_MARGIN);
    }
}

/* ---------------- layout 1: CALENDAR ONLY ---------------------------- */

static void build_calendar_only(void) {
    /* Packed (sizing pass 2026-11-22): grid_w 0 -> rows keep exactly
     * CAL_GAP col air (no SPACE_BETWEEN spread), header_w 0 -> the
     * widget sizes the header to the packed lattice (7*cell + 6*gap),
     * so year/month sit flush with the first and last column. The
     * whole block is content-sized and dead-centred on the page.
     * Revert note: CAL_HEADER_W x2 restored the full-bleed
     * SPACE_BETWEEN. */
    add_calendar(true, 0, 0, CAL_CELL_W, LV_ALIGN_CENTER, 0, 0);
}

/* ---------------- layout 2: FUSION ----------------------------------- */

static void build_fusion(void) {
    /* Stacked clock (hour over minute) centred in the space the grid
     * leaves (196 px): with the centre there, the clock's gaps to the
     * left margin and to the calendar are equal for any digit widths.
     * One placement mechanism only (the widget's align): a flex child
     * that is ALSO align-moved is what made the clock lean right. */
    lv_obj_t *zone = lv_obj_create(clock_root);

    lv_obj_remove_style_all(zone);
    lv_obj_set_size(zone, CAL_HEADER_W - CAL_FUSION_W, CAL_H);
    lv_obj_align(zone, LV_ALIGN_TOP_LEFT, ZMK_GRID_MARGIN, ZMK_GRID_MARGIN);

    add_clock(zone, ZMK_CLOCK_SIZE_STACKED, LV_ALIGN_CENTER, 0, 0);
    add_calendar(false, CAL_FUSION_W, CAL_FUSION_W, 0, LV_ALIGN_RIGHT_MID, -ZMK_GRID_MARGIN, 0);
}

static void clock_build_layout(void) {
    /* The page container belongs to the screen manager (it owns the page
     * dots as siblings); clean only our own subtree. */
    lv_obj_clean(clock_root);
    clock_w = NULL;
    cal_w = NULL;
    l_date = NULL;
    date_cache[0] = '\0';

#if IS_ENABLED(CONFIG_ZMK_DISPLAY_ANIM)
    /* a12: animated background (pad10a parity). Created before any
     * widget so it paints underneath; not clickable, so the tap
     * detector and swipes are untouched. */
    if (zmk_anim_bg_enabled() && zmk_anim_available("animbg")) {
        zmk_anim_create(clock_root, "animbg");
    }
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

    /* Nothing below the content band: the wave-1 audit dropped the
     * hint + unsync captions, leaving only the screen manager's page
     * dots at -MARGIN/2. */

    /* Fresh labels: seed everything from the live time now. */
    clock_update_text();
}

/* --------------------------------------------------------------------- */
/* One-second update                                                     */
/* --------------------------------------------------------------------- */

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

    /* a09 acceptance note: label updates must not starve touch. Full
     * cost per tick, averaged every 600 s — the panel repaint itself is
     * the fixed ~61 ms full frame (R5), independent of this. */
    clock_update_text();
    cycle_sum += k_cycle_get_32() - start;
    if (++samples >= 600) {
        LOG_INF("clock update avg %u us over %u s",
                (unsigned int)k_cyc_to_us_floor32(cycle_sum / samples), samples);
        samples = cycle_sum = 0;
    }
}

/* --------------------------------------------------------------------- */
/* Config changes: settings save, rebuild, tap cycle, RPC seam           */
/* --------------------------------------------------------------------- */

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

/* Dev/bench cycle: each tap advances one field — with one clock style
 * and the calendar style not yet joined, that is the layout alone
 * (clock only → calendar → fusion). The old sync toggle left with the
 * unsynced affordance: the flag has no visual anymore (host-side
 * zmk_time_sync() stays available for h02/a14). */
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

/* --------------------------------------------------------------------- */
/* Page registration / standalone screen                                 */
/* --------------------------------------------------------------------- */

#if IS_ENABLED(CONFIG_ZMK_DISPLAY_SCREEN_MGR)
/* Compile-time bound (see stats_screen.c): the global input callback can
 * fire long before this page is ever created. */
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

/* Tap (not swipe) on the clock page cycles one config field. The input
 * callback runs in the input reporting context; the LVGL work is posted to
 * the display work queue. Swipes travel far, so requiring near-zero
 * movement keeps the manager's page navigation untouched. */
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

/* --------------------------------------------------------------------- */
/* Settings                                                              */
/* --------------------------------------------------------------------- */

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

    /* Per-field defaults instead of rejecting the whole blob: an id from
     * a newer/older enum still leaves the other choices intact. */
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
/* Standalone keeps tap-to-cycle-theme from before the manager (see the
 * non-manager handler below the legacy section); with the manager the tap
 * drives the config cycle above. */
#else
static void clock_theme_cycle_work_cb(struct k_work *work) {
    ARG_UNUSED(work);

    /* The touch controller reports a phantom touch at power-on, so a cycle
     * work item can run before the screen exists — skip it (the theme
     * itself is always valid; this guard only orders against boot noise). */
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

/*
 * Touch handling: the input callback runs in the input reporting context, so
 * the LVGL update is posted to the display work queue.
 */
static void clock_random_color_work_cb(struct k_work *work) {
    ARG_UNUSED(work);

    /* The labels are created by zmk_display_status_screen() in this same
     * work queue. The touch controller reports a phantom touch at power-on,
     * so a color-change work item can run before the screen exists — skip
     * it rather than dereference NULL. */
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
