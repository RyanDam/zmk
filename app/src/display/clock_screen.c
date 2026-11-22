/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Clock and calendar screen for display-centric boards (Coban Tab 1a).
 *
 * a09: the pad10a time screen (graphics/screens/screen_time.[ch]) ported to
 * the panel's 480x320 landscape canvas (the 320x480 glass is turned by the
 * node's `rotation`; see grid.md). Its five time styles become five LVGL layouts
 * on the a07 grid (pad10a itself only distinguished three fonts and let
 * styles 4/5 fall back); its 12/24 h + AM/PM + three date formats + date
 * visibility enums are ported value-for-value (screen_time.h:29-42) so the
 * a14 RPC can reuse the ids. The format settings persist via ZMK settings
 * ("display/clock/format", one 5-byte blob — same field order as pad10a's
 * eeprom config).
 *
 * Time comes from the time-source seam (zmk/display/time_src.h), never from
 * uptime directly: the simulation reports synced = false and the screen
 * dims the hero digits + shows an "unsynced" mark until h02 (DS1302) or a14
 * (host set_time) confirms it.
 *
 * Dev/bench cycle: with the screen manager (a08) a tap ON the clock page
 * (not a swipe) advances one format field — style → time format → AM/PM →
 * date format → date visibility → sync toggle — so every acceptance combo
 * is reachable on the glass without a host; the hint caption names the
 * current combo. Standalone (no manager) keeps the old tap-cycles-theme.
 *
 * Styling rule (a07): every color/font/size comes from the style registry;
 * the legacy no-design-system path keeps the original hardcoded demo.
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
#include <zmk/display/status_screen.h>
#include <zmk/display/time_src.h>

#if IS_ENABLED(CONFIG_ZMK_DISPLAY_DESIGN_SYSTEM)
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
/* Format model (pad10a screen_time.h enums, ids ported unchanged)       */
/* --------------------------------------------------------------------- */

static const char *const weekday_full[] = {"Sunday",   "Monday", "Tuesday", "Wednesday",
                                           "Thursday", "Friday", "Saturday"};
static const char *const month_full[] = {"January",   "February", "March",    "April",
                                         "May",       "June",     "July",     "August",
                                         "September", "October",  "November", "December"};

enum clock_style {
    CLOCK_STYLE_HERO = 0,    /* 72 sans hero, seconds under          */
    CLOCK_STYLE_SERIF = 1,   /* DM Serif hero, date on top           */
    CLOCK_STYLE_STACKED = 2, /* hour over minute, weekday on top     */
    CLOCK_STYLE_CALENDAR = 3 /* month grid + time, "stationery" view */
    ,
    CLOCK_STYLE_DESK = 4, /* weekday + tabular date, big seconds  */
    CLOCK_STYLE_COUNT,
};

enum clock_time_format { CLOCK_FORMAT_12H = 0, CLOCK_FORMAT_24H = 1 };
enum clock_time_indicator { CLOCK_IND_DEFAULT = 0, CLOCK_IND_AMPM = 1 };
enum clock_date_format {
    CLOCK_DATE_DDMMYYYY = 0,
    CLOCK_DATE_MMDDYYYY = 1,
    CLOCK_DATE_YYYYMMDD = 2,
    CLOCK_DATE_FORMAT_COUNT,
};
enum clock_date_visibility { CLOCK_DATE_VISIBLE = 0, CLOCK_DATE_HIDDEN = 1 };

struct clock_fmt {
    uint8_t style;
    uint8_t time_format;
    uint8_t time_indicator;
    uint8_t date_format;
    uint8_t date_visibility;
};

static struct clock_fmt fmt = {
    .style = CONFIG_ZMK_DISPLAY_CLOCK_STYLE,
    .time_format = CONFIG_ZMK_DISPLAY_CLOCK_FORMAT,
    .time_indicator = CONFIG_ZMK_DISPLAY_CLOCK_INDICATOR,
    .date_format = CONFIG_ZMK_DISPLAY_CLOCK_DATE_FORMAT,
    .date_visibility =
        IS_ENABLED(CONFIG_ZMK_DISPLAY_CLOCK_DATE_VISIBLE) ? CLOCK_DATE_VISIBLE : CLOCK_DATE_HIDDEN,
};

/* --------------------------------------------------------------------- */
/* Format persistence (settings subtree "display/clock")                 */
/* --------------------------------------------------------------------- */

#if IS_ENABLED(CONFIG_SETTINGS)
#define CLOCK_SETTINGS_SUBTREE "display/clock"
#define CLOCK_SETTINGS_FMT_KEY "format"
#endif

/* --------------------------------------------------------------------- */
/* Widgets + layout builders                                             */
/* --------------------------------------------------------------------- */

/* Live labels of the current style; NULL where the style has no such
 * element, so the one-second updater can stay style-agnostic. */
static lv_obj_t *l_weekday, *l_cal_title, *l_hero, *l_hero2, *l_seconds, *l_indicator, *l_date,
    *l_unsync, *l_hint;
static lv_obj_t *cal_box;    /* month grid container (calendar style) */
static lv_obj_t *clock_root; /* content container, NULL until first built */

/* Hero style ids so the unsynced dim/undim can remove/add the right style;
 * a *_dim_id equal to its base id means "this element never dims"
 * (serif captions are already secondary-colored). */
static enum zmk_display_style_id hero_style_id, hero_dim_id, seconds_style_id, seconds_dim_id;
static bool hero_dimmed;

static int32_t last_date_key; /* yyyymmdd; calendar rebuilds on change */

static void clock_build_layout(void);
static void clock_update_hint(void);

/* Style-swap helper is clock-specific (registry style ids); the shared
 * label/flex/tap plumbing lives in ui_helpers.h. */

static void style_swap(lv_obj_t *obj, enum zmk_display_style_id from,
                       enum zmk_display_style_id to) {
    if (obj == NULL) {
        return;
    }
    lv_obj_remove_style(obj, zmk_display_style(from), 0);
    lv_obj_add_style(obj, zmk_display_style(to), 0);
}

/* Dim the hero ramp while the time source reports unsynced (a09's
 * "visible unsynced state"); re-applied after every layout/theme rebuild
 * because the style objects are rebuilt in place by the theme engine. */
static void clock_apply_dim(bool synced) {
    bool dim = !synced;

    if (dim == hero_dimmed) {
        return;
    }
    hero_dimmed = dim;

    /* Freshly built labels carry the base styles (hero_dimmed starts
     * false), and later transitions simply swap in the other direction. */
    if (l_hero != NULL && hero_dim_id != hero_style_id) {
        style_swap(l_hero, dim ? hero_style_id : hero_dim_id, dim ? hero_dim_id : hero_style_id);
    }
    if (l_hero2 != NULL) {
        style_swap(l_hero2, dim ? ZMK_STYLE_TEXT_SUBHERO : ZMK_STYLE_TEXT_SUBHERO_DIM,
                   dim ? ZMK_STYLE_TEXT_SUBHERO_DIM : ZMK_STYLE_TEXT_SUBHERO);
    }
    if (l_seconds != NULL && seconds_dim_id != seconds_style_id) {
        style_swap(l_seconds, dim ? seconds_style_id : seconds_dim_id,
                   dim ? seconds_dim_id : seconds_style_id);
    }
}

/* ---------------- style 0: HERO — the a08 default look, refined ------- */

static void build_hero(void) {
    lv_obj_t *col = zmk_ui_flex(clock_root, LV_FLEX_FLOW_COLUMN, ZMK_GRID_UNIT);

    /* Landscape (480x320): the column runs hero → seconds → AM/PM → date,
     * ~180 px tall; nudge it above centre so the -72/-32 caption slots stay
     * clear. */
    lv_obj_align(col, LV_ALIGN_CENTER, 0, -ZMK_GRID_UNIT * 3);

    l_hero = zmk_ui_label(col, ZMK_STYLE_TEXT_HERO);
    l_seconds = zmk_ui_label(col, ZMK_STYLE_TEXT_SUBHERO);
    l_indicator = zmk_ui_label(col, ZMK_STYLE_TEXT_CAPTION);
    l_date = zmk_ui_label(col, ZMK_STYLE_TEXT_SECONDARY);

    hero_style_id = ZMK_STYLE_TEXT_HERO;
    hero_dim_id = ZMK_STYLE_TEXT_HERO_DIM;
    seconds_style_id = ZMK_STYLE_TEXT_SUBHERO;
    seconds_dim_id = ZMK_STYLE_TEXT_SUBHERO_DIM;
}

/* ---------------- style 1: SERIF — DM Serif hero, date on top --------- */

static void build_serif(void) {
    lv_obj_t *top, *col, *row;

    top = zmk_ui_flex(clock_root, LV_FLEX_FLOW_COLUMN, ZMK_GRID_UNIT / 2);
    lv_obj_align(top, LV_ALIGN_TOP_MID, 0, ZMK_GRID_MARGIN + ZMK_GRID_UNIT);
    l_date = zmk_ui_label(top, ZMK_STYLE_TEXT_SECONDARY);

    col = zmk_ui_flex(clock_root, LV_FLEX_FLOW_COLUMN, ZMK_GRID_UNIT);
    lv_obj_align(col, LV_ALIGN_CENTER, 0, -ZMK_GRID_UNIT * 2);
    l_hero = zmk_ui_label(col, ZMK_STYLE_TEXT_HERO_ALT);

    row = zmk_ui_flex(col, LV_FLEX_FLOW_ROW, ZMK_GRID_UNIT * 2);
    l_seconds = zmk_ui_label(row, ZMK_STYLE_TEXT_CAPTION);
    l_indicator = zmk_ui_label(row, ZMK_STYLE_TEXT_CAPTION);

    hero_style_id = ZMK_STYLE_TEXT_HERO_ALT; /* no serif-dim font: hero stays lit */
    hero_dim_id = ZMK_STYLE_TEXT_HERO_ALT;
    seconds_style_id = ZMK_STYLE_TEXT_CAPTION;
    seconds_dim_id = ZMK_STYLE_TEXT_CAPTION;
}

/* ---------------- style 2: STACKED — hour over minute ----------------- */

static void build_stacked(void) {
    lv_obj_t *col, *row;

    l_weekday = zmk_ui_label(clock_root, ZMK_STYLE_TEXT_TITLE);
    lv_obj_align(l_weekday, LV_ALIGN_TOP_MID, 0, ZMK_GRID_MARGIN + ZMK_GRID_UNIT);

    col = zmk_ui_flex(clock_root, LV_FLEX_FLOW_COLUMN, ZMK_GRID_UNIT);
    lv_obj_align(col, LV_ALIGN_CENTER, 0, -ZMK_GRID_UNIT * 2);
    l_hero = zmk_ui_label(col, ZMK_STYLE_TEXT_HERO);     /* hour   */
    l_hero2 = zmk_ui_label(col, ZMK_STYLE_TEXT_SUBHERO); /* minute */
    row = zmk_ui_flex(col, LV_FLEX_FLOW_ROW, ZMK_GRID_UNIT * 2);
    l_seconds = zmk_ui_label(row, ZMK_STYLE_TEXT_CAPTION);
    l_indicator = zmk_ui_label(row, ZMK_STYLE_TEXT_CAPTION);
    /* date joins the column: at 320 px tall there is no clear space below
     * it for the portrait-era centre offset. */
    l_date = zmk_ui_label(col, ZMK_STYLE_TEXT_SECONDARY);

    hero_style_id = ZMK_STYLE_TEXT_HERO;
    hero_dim_id = ZMK_STYLE_TEXT_HERO_DIM;
    seconds_style_id = ZMK_STYLE_TEXT_CAPTION;
    seconds_dim_id = ZMK_STYLE_TEXT_CAPTION;
}

/* ---------------- style 3: CALENDAR — month grid ---------------------- */

static int days_in_month(uint8_t month, uint16_t year) {
    static const uint8_t lengths[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int days = lengths[month - 1];

    if (month == 2 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))) {
        days++;
    }
    return days;
}

static void build_calendar_cells(const struct zmk_time *t) {
    static const char headers[] = "SMTWTFS";
    /* Weekday of the 1st, derived from today's (0 = Sunday). */
    int first_wd = (t->weekday + 7 - ((t->day - 1) % 7)) % 7;
    int dim = days_in_month(t->month, t->year);

    lv_obj_clean(cal_box);

    /* weekday header row, then 6 week rows; 40 px cells fit the 288 px
     * content width with the safe margins. */
    lv_obj_t *hrow = zmk_ui_flex(cal_box, LV_FLEX_FLOW_ROW, 0);

    for (int c = 0; c < 7; c++) {
        lv_obj_t *lbl = zmk_ui_label(hrow, ZMK_STYLE_TEXT_CAPTION);
        char ch[2] = {headers[c], '\0'};

        lv_label_set_text(lbl, ch);
        lv_obj_set_size(lbl, 40, 20);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    }

    for (int r = 0; r < 6; r++) {
        lv_obj_t *wrow = zmk_ui_flex(cal_box, LV_FLEX_FLOW_ROW, 0);

        for (int c = 0; c < 7; c++) {
            int day = r * 7 + c - first_wd + 1;

            if (day < 1 || day > dim) {
                lv_obj_t *pad = lv_obj_create(wrow);

                lv_obj_remove_style_all(pad);
                lv_obj_set_size(pad, 40, 28);
                continue;
            }

            if (day == t->day) {
                /* Today: accent pip, text from the ON_ACCENT style. */
                lv_obj_t *pip = lv_obj_create(wrow);

                lv_obj_remove_style_all(pip);
                lv_obj_set_size(pip, 28, 28);
                lv_obj_set_style_pad_all(pip, 0, 0);
                lv_obj_add_style(pip, zmk_display_style(ZMK_STYLE_ACCENT), 0);
                lv_obj_set_style_radius(pip, LV_RADIUS_CIRCLE, 0);

                lv_obj_t *lbl = lv_label_create(pip);

                lv_obj_add_style(lbl, zmk_display_style(ZMK_STYLE_TEXT_ON_ACCENT), 0);
                lv_obj_center(lbl);
                lv_label_set_text_fmt(lbl, "%d", day);
            } else {
                lv_obj_t *lbl = zmk_ui_label(wrow, ZMK_STYLE_TEXT_TABULAR);

                lv_obj_set_size(lbl, 40, 28);
                lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
                lv_label_set_text_fmt(lbl, "%d", day);
            }
        }
    }
}

static void build_calendar(void) {
    lv_obj_t *left, *trow;

    l_cal_title = zmk_ui_label(clock_root, ZMK_STYLE_TEXT_TITLE);
    lv_obj_align(l_cal_title, LV_ALIGN_TOP_MID, 0, ZMK_GRID_MARGIN + ZMK_GRID_UNIT);

    /* Landscape split: time on the left, month grid on the right (the grid
     * is 280x188 - half the wide screen is exactly its room). */
    left = zmk_ui_flex(clock_root, LV_FLEX_FLOW_COLUMN, ZMK_GRID_UNIT / 2);
    lv_obj_align(left, LV_ALIGN_LEFT_MID, ZMK_GRID_MARGIN, 0);
    l_hero = zmk_ui_label(left, ZMK_STYLE_TEXT_SUBHERO);

    trow = zmk_ui_flex(left, LV_FLEX_FLOW_ROW, ZMK_GRID_UNIT * 2);
    l_seconds = zmk_ui_label(trow, ZMK_STYLE_TEXT_CAPTION);
    l_indicator = zmk_ui_label(trow, ZMK_STYLE_TEXT_CAPTION);

    cal_box = zmk_ui_flex(clock_root, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_align(cal_box, LV_ALIGN_RIGHT_MID, -ZMK_GRID_MARGIN, 0);

    l_date = zmk_ui_label(clock_root, ZMK_STYLE_TEXT_SECONDARY);
    lv_obj_align(l_date, LV_ALIGN_BOTTOM_LEFT, ZMK_GRID_MARGIN, -ZMK_GRID_MARGIN);

    hero_style_id = ZMK_STYLE_TEXT_SUBHERO;
    hero_dim_id = ZMK_STYLE_TEXT_SUBHERO_DIM;
    seconds_style_id = ZMK_STYLE_TEXT_CAPTION;
    seconds_dim_id = ZMK_STYLE_TEXT_CAPTION;
    last_date_key = 0; /* force the first cell build */
}

/* ---------------- style 4: DESK — tabular date, big seconds ----------- */

static void build_desk(void) {
    lv_obj_t *trow;

    l_weekday = zmk_ui_label(clock_root, ZMK_STYLE_TEXT_TITLE);
    lv_obj_align(l_weekday, LV_ALIGN_TOP_MID, 0, ZMK_GRID_MARGIN + ZMK_GRID_UNIT);

    trow = zmk_ui_flex(clock_root, LV_FLEX_FLOW_ROW, ZMK_GRID_UNIT * 2);
    lv_obj_align(trow, LV_ALIGN_CENTER, 0, -ZMK_GRID_UNIT * 4);
    l_hero = zmk_ui_label(trow, ZMK_STYLE_TEXT_HERO);
    l_seconds = zmk_ui_label(trow, ZMK_STYLE_TEXT_SUBHERO);

    l_indicator = zmk_ui_label(clock_root, ZMK_STYLE_TEXT_CAPTION);
    lv_obj_align(l_indicator, LV_ALIGN_CENTER, 0, ZMK_GRID_UNIT * 3);

    l_date = zmk_ui_label(clock_root, ZMK_STYLE_TEXT_TABULAR);
    lv_obj_align(l_date, LV_ALIGN_CENTER, 0, ZMK_GRID_UNIT * 6);

    hero_style_id = ZMK_STYLE_TEXT_HERO;
    hero_dim_id = ZMK_STYLE_TEXT_HERO_DIM;
    seconds_style_id = ZMK_STYLE_TEXT_SUBHERO;
    seconds_dim_id = ZMK_STYLE_TEXT_SUBHERO_DIM;
}

static void clock_build_layout(void) {
    /* The page container belongs to the screen manager (it owns the page
     * dots as siblings); clean only our own subtree. */
    lv_obj_clean(clock_root);
    l_weekday = l_cal_title = l_hero = l_hero2 = l_seconds = NULL;
    l_indicator = l_date = l_unsync = l_hint = cal_box = NULL;
    hero_dimmed = false;

#if IS_ENABLED(CONFIG_ZMK_DISPLAY_ANIM)
    /* a12: animated background (pad10a parity). Created before any
     * widget so it paints underneath; not clickable, so the tap
     * detector and swipes are untouched. */
    if (zmk_anim_bg_enabled() && zmk_anim_available("animbg")) {
        zmk_anim_create(clock_root, "animbg");
    }
#endif

    switch (fmt.style) {
    case CLOCK_STYLE_SERIF:
        build_serif();
        break;
    case CLOCK_STYLE_STACKED:
        build_stacked();
        break;
    case CLOCK_STYLE_CALENDAR:
        build_calendar();
        break;
    case CLOCK_STYLE_DESK:
        build_desk();
        break;
    case CLOCK_STYLE_HERO:
    default:
        build_hero();
        break;
    }

    /* Shared bottom furniture, above the page dots (landscape: caption
     * slots at -72 and -32, dots live at -MARGIN/2). */
    l_unsync = zmk_ui_label(clock_root, ZMK_STYLE_TEXT_CAPTION);
    lv_obj_align(l_unsync, LV_ALIGN_BOTTOM_MID, 0, -(ZMK_GRID_MARGIN * 4 + ZMK_GRID_UNIT));
    lv_label_set_text(l_unsync, "unsynced");

    l_hint = zmk_ui_label(clock_root, ZMK_STYLE_TEXT_CAPTION);
    lv_obj_align(l_hint, LV_ALIGN_BOTTOM_MID, 0, -ZMK_GRID_MARGIN * 2);

    /* Fresh labels carry the base styles; dim them now if the time source
     * is (still) unsynced. */
    struct zmk_time now;
    bool synced = (zmk_time_now(&now) == 0) && now.synced;

    hero_dimmed = false;
    clock_apply_dim(synced);
}

/* --------------------------------------------------------------------- */
/* One-second update                                                     */
/* --------------------------------------------------------------------- */

static void fmt_time_str(const struct zmk_time *t, char *buf, size_t size) {
    uint8_t hour = (fmt.time_format == CLOCK_FORMAT_12H) ? (t->hour % 12) : t->hour;

    snprintk(buf, size, "%02u:%02u", hour, t->minute);
}

static void fmt_date_str(const struct zmk_time *t, char *buf, size_t size) {
    switch (fmt.date_format) {
    case CLOCK_DATE_MMDDYYYY:
        snprintk(buf, size, "%02u / %02u / %04u", t->month, t->day, t->year);
        break;
    case CLOCK_DATE_YYYYMMDD:
        snprintk(buf, size, "%04u / %02u / %02u", t->year, t->month, t->day);
        break;
    case CLOCK_DATE_DDMMYYYY:
    default:
        snprintk(buf, size, "%02u / %02u / %04u", t->day, t->month, t->year);
        break;
    }
}

static void clock_update_text(void) {
    static char ind_cache[8], date_cache[32], wday_cache[16], unsync_cache[12],
        cal_cache[32]; /* last-drawn text: unchanged -> no invalidation */
    struct zmk_time t;
    char buf[32];

    if (zmk_time_now(&t) != 0 || l_hero == NULL) {
        return;
    }

    if (fmt.style == CLOCK_STYLE_STACKED) {
        snprintk(buf, sizeof(buf), "%02u",
                 (fmt.time_format == CLOCK_FORMAT_12H) ? (t.hour % 12) : t.hour);
        lv_label_set_text(l_hero, buf);
        snprintk(buf, sizeof(buf), "%02u", t.minute);
        lv_label_set_text(l_hero2, buf);
    } else {
        fmt_time_str(&t, buf, sizeof(buf));
        lv_label_set_text(l_hero, buf);
    }

    if (l_seconds != NULL) {
        snprintk(buf, sizeof(buf), ":%02u", t.second);
        lv_label_set_text(l_seconds, buf);
    }

    if (l_indicator != NULL) {
        /* pad10a semantics (screen_time.c:85-99): shown per the indicator
         * setting, independent of the 12/24 h format. */
        zmk_ui_set_text(ind_cache, sizeof(ind_cache), l_indicator,
                        (fmt.time_indicator == CLOCK_IND_AMPM) ? (t.hour <= 12 ? "AM" : "PM") : "");
    }

    if (l_date != NULL) {
        if (fmt.date_visibility == CLOCK_DATE_VISIBLE) {
            fmt_date_str(&t, buf, sizeof(buf));
        } else {
            buf[0] = '\0';
        }
        zmk_ui_set_text(date_cache, sizeof(date_cache), l_date, buf);
    }

    if (l_weekday != NULL) {
        zmk_ui_set_text(wday_cache, sizeof(wday_cache), l_weekday, weekday_full[t.weekday]);
    }

    clock_apply_dim(t.synced);
    if (l_unsync != NULL) {
        zmk_ui_set_text(unsync_cache, sizeof(unsync_cache), l_unsync, t.synced ? "" : "unsynced");
    }

    if (fmt.style == CLOCK_STYLE_CALENDAR) {
        int32_t key = (int32_t)t.year * 10000 + t.month * 100 + t.day;

        if (key != last_date_key) {
            last_date_key = key;
            build_calendar_cells(&t);
            snprintk(buf, sizeof(buf), "%s %u", month_full[t.month - 1], t.year);
            zmk_ui_set_text(cal_cache, sizeof(cal_cache), l_cal_title, buf);
        }
    }
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
/* Format changes: settings save, tap cycle, rebuild                     */
/* --------------------------------------------------------------------- */

ZMK_UI_WORK_DEFINE(clock_rebuild_work, clock_rebuild_work_cb) {
    ARG_UNUSED(work);

    if (clock_root == NULL) {
        return; /* page not built yet; the next create() uses fmt as-is */
    }
    clock_build_layout();
    clock_update_hint();
    clock_update_text();
}

static void clock_schedule_rebuild(void) { zmk_ui_post(&clock_rebuild_work); }

#if IS_ENABLED(CONFIG_SETTINGS)
static void clock_settings_save(void) {
    zmk_settings_blob_save(CLOCK_SETTINGS_SUBTREE "/" CLOCK_SETTINGS_FMT_KEY, &fmt, sizeof(fmt),
                           "clock format");
}
#endif

static void clock_update_hint(void) {
    static const char *const date_names[] = {"DD/MM", "MM/DD", "YYYY/MM"};
    static char hint_cache[48];
    char buf[48];

    if (l_hint == NULL) {
        return;
    }
    snprintk(buf, sizeof(buf), "S%u %s %s - tap to tune", (unsigned int)fmt.style + 1,
             (fmt.time_format == CLOCK_FORMAT_12H) ? "12h" : "24h",
             date_names[MIN((unsigned int)fmt.date_format, CLOCK_DATE_FORMAT_COUNT - 1)]);
    zmk_ui_set_text(hint_cache, sizeof(hint_cache), l_hint, buf);
}

/* Dev/bench cycle: each tap advances one field, so every acceptance combo
 * (5 styles x 2 formats x AM/PM x 3 dates x visibility x sync) is
 * reachable on the glass. */
#define CLOCK_DEV_FIELDS 6
static uint8_t dev_field;

ZMK_UI_WORK_DEFINE(clock_cycle_field_work, clock_cycle_field_work_cb) {
    ARG_UNUSED(work);

    switch (dev_field) {
    case 0:
        fmt.style = (fmt.style + 1) % CLOCK_STYLE_COUNT;
        break;
    case 1:
        fmt.time_format ^= 1;
        break;
    case 2:
        fmt.time_indicator ^= 1;
        break;
    case 3:
        fmt.date_format = (fmt.date_format + 1) % CLOCK_DATE_FORMAT_COUNT;
        break;
    case 4:
        fmt.date_visibility ^= 1;
        break;
    case 5:
        /* Sync toggle: the seam test hook until h02/a14 land. */
        {
            bool synced;
            int64_t epoch = zmk_time_now_epoch(&synced);

            if (synced) {
                zmk_time_mark_unsynced();
            } else {
                zmk_time_sync(epoch);
            }
            break;
        }
    }

    LOG_INF("clock fmt: style %u format %u indicator %u date %u vis %u (field %u cycled)",
            fmt.style, fmt.time_format, fmt.time_indicator, fmt.date_format, fmt.date_visibility,
            dev_field);
    dev_field = (dev_field + 1) % CLOCK_DEV_FIELDS;

#if IS_ENABLED(CONFIG_SETTINGS)
    clock_settings_save();
#endif
    clock_schedule_rebuild();
}

/* --------------------------------------------------------------------- */
/* Page registration / standalone screen                                 */
/* --------------------------------------------------------------------- */

#if IS_ENABLED(CONFIG_ZMK_DISPLAY_SCREEN_MGR)
/* Compile-time bound (see stats_screen.c): the global input callback can
 * fire long before this page is ever created. */
ZMK_UI_TAP_STATE(clock_tap) = { .page_name = "clock", .work = &clock_cycle_field_work };
#endif

static lv_obj_t *clock_attach(lv_obj_t *parent) {
    clock_root = lv_obj_create(parent);

    lv_obj_remove_style_all(clock_root);
    lv_obj_set_size(clock_root, lv_pct(100), lv_pct(100));

    clock_build_layout();
    clock_update_hint();
    clock_update_text();
    zmk_ui_page_timer("clock", 1000, clock_tick);

    return parent;
}

#if IS_ENABLED(CONFIG_ZMK_DISPLAY_SCREEN_MGR)

static lv_obj_t *clock_page_create(lv_obj_t *parent) { return clock_attach(parent); }

ZMK_DISPLAY_DEFINE_SCREEN(clock, "clock", clock_page_create, 10);

/* Tap (not swipe) on the clock page cycles one format field. The input
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
static int clock_settings_handle_set(const char *key, size_t len, settings_read_cb read_cb,
                                     void *cb_arg) {
    struct clock_fmt loaded;
    int err = zmk_settings_blob_read(key, len, read_cb, cb_arg, CLOCK_SETTINGS_FMT_KEY, &loaded,
                                     sizeof(loaded), "clock format");

    if (err <= 0) {
        return err;
    }

    if (loaded.style >= CLOCK_STYLE_COUNT || loaded.time_format > CLOCK_FORMAT_24H ||
        loaded.time_indicator > CLOCK_IND_AMPM || loaded.date_format >= CLOCK_DATE_FORMAT_COUNT ||
        loaded.date_visibility > CLOCK_DATE_HIDDEN) {
        LOG_WRN("clock format settings out of range - ignored");
        return -EINVAL;
    }

    fmt = loaded;
    clock_schedule_rebuild();
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(display_clock, CLOCK_SETTINGS_SUBTREE, NULL,
                               clock_settings_handle_set, NULL, NULL);
#endif /* CONFIG_SETTINGS */

#if IS_ENABLED(CONFIG_ZMK_DISPLAY_SCREEN_MGR)
/* Standalone keeps tap-to-cycle-theme from before the manager (see the
 * non-manager handler below the legacy section); with the manager the tap
 * drives the format cycle above. */
#else
static void clock_theme_cycle_work_cb(struct k_work *work) {
    ARG_UNUSED(work);

    /* The touch controller reports a phantom touch at power-on, so a cycle
     * work item can run before the screen exists — skip it (the theme
     * itself is always valid; this guard only orders against boot noise). */
    if (l_hero == NULL) {
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
