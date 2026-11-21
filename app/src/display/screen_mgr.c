/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Screen manager + touch shell implementation (backlog a08).
 *
 * Pad 10a had these behaviors split across housekeeping_task_user and
 * graphics/ui.h (screen id enum + change_screen, deferred home restore,
 * idle backlight-off with pomodoro suppression — pad10a.c:81-150,
 * graphics/ui.h:33-46). Here they become one ZMK/LVGL module whose activity
 * signal is touch, not keypress, because this board has no keys yet (h04).
 *
 * Navigation is gesture-based but NOT LVGL-gesture-based: LVGL 9.5 only
 * emits LV_EVENT_GESTURE when CONFIG_LV_USE_GESTURE_RECOGNITION (and
 * LV_USE_FLOAT) is on, and the panel's full-refresh ~16fps ceiling (risk
 * R5) makes a flick-following scroll container repaint worse than a single
 * discrete move animation. So the swipe is recognized from the raw Zephyr
 * input events of the touch device (which also reset the idle timer — the
 * same events feed LVGL's pointer via LV_Z_POINTER_FROM_CHOSEN_TOUCH).
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <lvgl.h>

#include <zmk/display.h>
#include <zmk/display/screens.h>

#include "styles.h"
#include "ui_helpers.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define MAX_SCREENS CONFIG_ZMK_DISPLAY_SCREEN_MGR_MAX_SCREENS

/* A swipe must travel this far horizontally and be mostly horizontal. */
#define SWIPE_MIN_DX (ZMK_GRID_UNIT * 8) /* 64 px */

/* ------------------------------------------------------------------ */
/* Screen registry (sorted view of the linker section)                 */
/* ------------------------------------------------------------------ */

static const struct zmk_display_screen *registry[MAX_SCREENS];
static int registry_count;

static int screen_find_locked(const char *name) {
    for (int i = 0; i < registry_count; i++) {
        if (strcmp(registry[i]->name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static void registry_collect(void) {
    const struct zmk_display_screen *s;
    int insertion, shift;

    registry_count = 0;
    STRUCT_SECTION_FOREACH(zmk_display_screen, item) {
        if (registry_count >= MAX_SCREENS) {
            LOG_WRN("more than %d screens registered; extra ignored", MAX_SCREENS);
            break;
        }
        registry[registry_count++] = item;
    }

    /* Stable insertion sort by `order` so page order is deterministic
     * regardless of link order. */
    for (insertion = 1; insertion < registry_count; insertion++) {
        s = registry[insertion];
        for (shift = insertion; shift > 0 && registry[shift - 1]->order > s->order; shift--) {
            registry[shift] = registry[shift - 1];
        }
        registry[shift] = s;
    }

    for (int i = 0; i < registry_count; i++) {
        LOG_INF("screen[%d] = %s", i, registry[i]->name);
    }
}

int zmk_display_screen_count(void) { return registry_count; }

const struct zmk_display_screen *zmk_display_screen_get(int index) {
    if (index < 0 || index >= registry_count) {
        return NULL;
    }
    return registry[index];
}

int zmk_display_screen_find(const char *name) { return screen_find_locked(name); }

/* ------------------------------------------------------------------ */
/* Pages, page dots, and navigation (display work queue only)          */
/* ------------------------------------------------------------------ */

struct page {
    lv_obj_t *obj;               /* NULL until the page is first shown */
    lv_obj_t *dots[MAX_SCREENS]; /* page-dot pips, NULL if count == 1  */
};

static struct page pages[MAX_SCREENS];
static lv_obj_t *splash_page;

static atomic_t current_idx = ATOMIC_INIT(0);
static atomic_t pending_nav = ATOMIC_INIT(-1);
static struct k_work nav_work;

static void build_dots(struct page *page, int index) {
    int n = registry_count;
    int dot = ZMK_GRID_UNIT; /* 8 px pips                        */
    int gap = ZMK_GRID_UNIT; /* one grid unit between them       */
    int hor_res = lv_display_get_horizontal_resolution(lv_display_get_default());
    int total = n * dot + (n - 1) * gap;
    int x = (hor_res - total) / 2;

    for (int i = 0; i < n; i++) {
        lv_obj_t *pip = lv_obj_create(page->obj);
        lv_obj_remove_style_all(pip);
        lv_obj_set_size(pip, dot, dot);
        lv_obj_set_style_radius(pip, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(pip, LV_OPA_COVER, 0);
        /* Align each pip's centre relative to BOTTOM_MID: the flex-free math
         * keeps them pixel-exact on the full-refresh panel. */
        lv_obj_align(pip, LV_ALIGN_BOTTOM_MID, x + i * (dot + gap) + dot / 2 - hor_res / 2,
                     -ZMK_GRID_MARGIN / 2);
        lv_obj_add_style(pip,
                         i == index ? zmk_display_style(ZMK_STYLE_ACCENT)
                                    : zmk_display_style(ZMK_STYLE_INACTIVE),
                         0);
        page->dots[i] = pip;
    }
}

static void restyle_dots(int index) {
    for (int p = 0; p < MAX_SCREENS; p++) {
        struct page *page = &pages[p];
        if (page->obj == NULL || page->dots[0] == NULL) {
            continue;
        }
        for (int i = 0; i < registry_count; i++) {
            if (page->dots[i] == NULL) {
                continue;
            }
            lv_obj_remove_style(page->dots[i], zmk_display_style(ZMK_STYLE_ACCENT), 0);
            lv_obj_remove_style(page->dots[i], zmk_display_style(ZMK_STYLE_INACTIVE), 0);
            lv_obj_add_style(page->dots[i],
                             i == index ? zmk_display_style(ZMK_STYLE_ACCENT)
                                        : zmk_display_style(ZMK_STYLE_INACTIVE),
                             0);
        }
    }
}

static lv_obj_t *page_get(int index) {
    struct page *page = &pages[index];

    if (page->obj == NULL) {
        page->obj = lv_obj_create(NULL);
        lv_obj_add_style(page->obj, zmk_display_style(ZMK_STYLE_SCREEN), 0);
        registry[index]->create(page->obj);
        if (registry_count > 1) {
            build_dots(page, index);
        }
    } else if (registry_count > 1 && page->dots[0] != NULL) {
        restyle_dots(index);
    }

    return page->obj;
}

static void nav_apply(void) {
    int target = atomic_get(&pending_nav);

    atomic_set(&pending_nav, -1);
    if (target < 0 || target >= registry_count) {
        return;
    }

    int from = atomic_get(&current_idx);
    bool from_splash = splash_page != NULL;

    if (target == from && !from_splash) {
        return;
    }

    lv_obj_t *page = page_get(target);
    lv_screen_load_anim_t anim = LV_SCREEN_LOAD_ANIM_FADE_ON;

    if (from_splash) {
        /* First real navigation tears down the splash for good. */
        splash_page = NULL;
    } else if (target > from) {
        anim = LV_SCREEN_LOAD_ANIM_MOVE_LEFT;
    } else if (target < from) {
        anim = LV_SCREEN_LOAD_ANIM_MOVE_RIGHT;
    }

    /* Full-refresh panel: ~61 ms per frame, so the animation is short and
     * no easing can pretend otherwise (R5). auto_del frees the splash but
     * keeps the cached pages. */
    lv_screen_load_anim(page, anim, 250, 0, from_splash);
    atomic_set(&current_idx, target);
    restyle_dots(target);
    LOG_INF("screen -> %d (%s)", target, registry[target]->name);
}

static void nav_work_cb(struct k_work *work) {
    ARG_UNUSED(work);
    nav_apply();
}

int zmk_display_screen_current(void) { return atomic_get(&current_idx); }

int zmk_display_screen_goto(int index) {
    if (registry_count == 0) {
        return -ENODEV;
    }
    if (index < 0) {
        index = 0;
    }
    if (index >= registry_count) {
        index = registry_count - 1;
    }
    atomic_set(&pending_nav, index);
    k_work_submit_to_queue(zmk_display_work_q(), &nav_work);
    return 0;
}

int zmk_display_screen_next(void) {
    int n = registry_count;
    if (n == 0) {
        return -ENODEV;
    }
    return zmk_display_screen_goto((atomic_get(&current_idx) + 1) % n);
}

int zmk_display_screen_prev(void) {
    int n = registry_count;
    if (n == 0) {
        return -ENODEV;
    }
    return zmk_display_screen_goto((atomic_get(&current_idx) + n - 1) % n);
}

/* ------------------------------------------------------------------ */
/* Home screen persistence                                             */
/* ------------------------------------------------------------------ */

#if IS_ENABLED(CONFIG_SETTINGS)
#define SCREENS_SETTINGS_SUBTREE "display/screens"
#define SCREENS_SETTINGS_HOME_KEY "home"

static int32_t persisted_home_idx = -1;

static int screens_settings_handle_set(const char *key, size_t len, settings_read_cb read_cb,
                                       void *cb_arg) {
    if (strcmp(SCREENS_SETTINGS_HOME_KEY, key) != 0) {
        return 0; /* not ours */
    }

    if (len != sizeof(persisted_home_idx)) {
        LOG_WRN("bad home-screen settings size %zu", len);
        return -EINVAL;
    }

    int err = read_cb(cb_arg, &persisted_home_idx, sizeof(persisted_home_idx));
    if (err <= 0) {
        LOG_ERR("Failed to read home screen from settings (err %d)", err);
        return err;
    }

    LOG_INF("home screen loaded from settings: %d", (int)persisted_home_idx);
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(display_screens, SCREENS_SETTINGS_SUBTREE, NULL,
                               screens_settings_handle_set, NULL, NULL);

static void screens_settings_save(int index) {
    int32_t value = index;
    int err = settings_save_one(SCREENS_SETTINGS_SUBTREE "/" SCREENS_SETTINGS_HOME_KEY, &value,
                                sizeof(value));
    if (err) {
        LOG_WRN("Could not persist home screen (err %d) — live apply only", err);
    }
}
#else
static int32_t persisted_home_idx = -1;
static void screens_settings_save(int index) { ARG_UNUSED(index); }
#endif /* CONFIG_SETTINGS */

int zmk_display_screen_set_home(int index) {
    if (index < 0 || index >= registry_count) {
        return -EINVAL;
    }
    persisted_home_idx = index;
    screens_settings_save(index);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Idle backlight policy: full → dim → blank, wake on touch            */
/* ------------------------------------------------------------------ */

enum idle_state { IDLE_ACTIVE, IDLE_DIMMED, IDLE_BLANKED };

static atomic_t state = ATOMIC_INIT(IDLE_ACTIVE);
static atomic_t last_activity_ms;
static atomic_t blank_locks;
static struct k_work idle_work;

static void idle_timer_cb(struct k_timer *timer) {
    ARG_UNUSED(timer);
    k_work_submit_to_queue(zmk_display_work_q(), &idle_work);
}
K_TIMER_DEFINE(idle_timer, idle_timer_cb, NULL);

/*
 * Backlight PWM, straight from the panel node's `pwms` property (the AXS
 * driver owns only on/off through display blanking; dimming needs a
 * mid-level duty, and this is the one property both agree on). A 100 % duty
 * never lit the backlight through the LEDC channel idle level, so full on
 * is period-1 — same as the driver (axs15231b.c:228-248).
 */
#if DT_HAS_CHOSEN(zephyr_display) && DT_NODE_HAS_PROP(DT_CHOSEN(zephyr_display), pwms)
static const struct pwm_dt_spec backlight_pwm = PWM_DT_SPEC_GET(DT_CHOSEN(zephyr_display));
#define HAS_BACKLIGHT_PWM 1
#endif

static void backlight_set_percent(uint8_t percent) {
#if HAS_BACKLIGHT_PWM
    if (!pwm_is_ready_dt(&backlight_pwm)) {
        return;
    }

    uint32_t pulse = (percent >= 100) ? backlight_pwm.period - 1
                                      : (uint32_t)((uint64_t)backlight_pwm.period * percent / 100);
    int err = pwm_set_dt(&backlight_pwm, backlight_pwm.period, pulse);
    if (err) {
        LOG_WRN("backlight set %u%% failed (%d)", percent, err);
    }
#else
    ARG_UNUSED(percent);
#endif
}

static void set_state(enum idle_state next) {
    enum idle_state prev = atomic_get(&state);
    if (prev == next) {
        return;
    }

    switch (next) {
    case IDLE_ACTIVE:
        if (prev == IDLE_BLANKED) {
            zmk_display_updates_start();
        }
        backlight_set_percent(100);
        break;
    case IDLE_DIMMED:
        backlight_set_percent(CONFIG_ZMK_DISPLAY_DIM_LEVEL);
        break;
    case IDLE_BLANKED:
        zmk_display_updates_stop();
        break;
    }

    atomic_set(&state, next);
}

static void idle_work_cb(struct k_work *work) {
    ARG_UNUSED(work);

    uint32_t now = (uint32_t)k_uptime_get();
    int32_t idle = now - (uint32_t)atomic_get(&last_activity_ms);

    if (atomic_get(&blank_locks) > 0) {
        set_state(IDLE_ACTIVE);
        return;
    }

    if (idle >= CONFIG_ZMK_DISPLAY_BLANK_TIMEOUT_MS) {
        set_state(IDLE_BLANKED);
    } else if (idle >= CONFIG_ZMK_DISPLAY_DIM_TIMEOUT_MS) {
        set_state(IDLE_DIMMED);
    } else {
        set_state(IDLE_ACTIVE);
    }
}

static void touch_activity(void) {
    atomic_set(&last_activity_ms, (int32_t)(uint32_t)k_uptime_get());
    if (atomic_get(&state) != IDLE_ACTIVE) {
        k_work_submit_to_queue(zmk_display_work_q(), &idle_work);
    }
}

void zmk_display_backlight_wake(void) { touch_activity(); }

void zmk_display_blank_lock(void) {
    atomic_inc(&blank_locks);
    touch_activity();
}

void zmk_display_blank_unlock(void) {
    if (atomic_get(&blank_locks) > 0) {
        atomic_dec(&blank_locks);
    }
    /* Re-arm the timers from now so an unlock does not blank instantly. */
    touch_activity();
}

/* ------------------------------------------------------------------ */
/* Touch: swipe gestures + the activity signal                         */
/* ------------------------------------------------------------------ */

static struct {
    bool pressed;
    uint16_t x, y;
    uint16_t start_x, start_y;
} touch;

static void mgr_touch_cb(struct input_event *evt, void *user_data) {
    ARG_UNUSED(user_data);

    switch (evt->type) {
    case INPUT_EV_ABS:
        if (evt->code == INPUT_ABS_X) {
            touch.x = evt->value;
        } else if (evt->code == INPUT_ABS_Y) {
            touch.y = evt->value;
        } else {
            break;
        }
        touch_activity();
        if (!touch.pressed) {
            /* Coordinates can arrive with/before the press; latch a start
             * point so a report stream that starts mid-motion still works. */
            touch.start_x = touch.x;
            touch.start_y = touch.y;
        }
        break;

    case INPUT_EV_KEY:
        if (evt->code != INPUT_BTN_TOUCH) {
            break;
        }
        touch_activity();
        if (evt->value) {
            touch.pressed = true;
            touch.start_x = touch.x;
            touch.start_y = touch.y;
        } else if (touch.pressed) {
            int32_t dx = (int32_t)touch.x - (int32_t)touch.start_x;
            int32_t dy = (int32_t)touch.y - (int32_t)touch.start_y;
            int32_t adx = dx < 0 ? -dx : dx;
            int32_t ady = dy < 0 ? -dy : dy;

            touch.pressed = false;
            if (adx >= SWIPE_MIN_DX && adx >= 2 * ady) {
                /* Finger moves right = reveal the page to the left. */
                if (dx < 0) {
                    zmk_display_screen_next();
                } else {
                    zmk_display_screen_prev();
                }
            }
        }
        break;
    }
}
INPUT_CALLBACK_DEFINE(NULL, mgr_touch_cb, NULL);

/* ------------------------------------------------------------------ */
/* Boot splash → home                                                  */
/* ------------------------------------------------------------------ */

/* Implemented in boot_screen.c. */
lv_obj_t *zmk_display_boot_screen_create(void);

#define IDLE_POLICY_PERIOD_MS 250

static int splash_home_idx;
static struct k_work splash_done_work;

static void splash_done_work_cb(struct k_work *work) {
    ARG_UNUSED(work);
    atomic_set(&pending_nav, splash_home_idx);
    nav_apply();
}

static void splash_timer_cb(struct k_timer *timer) {
    ARG_UNUSED(timer);
    k_work_submit_to_queue(zmk_display_work_q(), &splash_done_work);
}
K_TIMER_DEFINE(splash_timer, splash_timer_cb, NULL);

/* ------------------------------------------------------------------ */
/* Init                                                                */
/* ------------------------------------------------------------------ */

int zmk_display_screen_mgr_init(void) {
    int home_idx = 0;

    zmk_ui_indev_init(); /* clickable widgets need LVGL pointer events */

    registry_collect();
    if (registry_count == 0) {
        LOG_ERR("screen manager: no screens registered");
        return -ENODEV;
    }

    if (persisted_home_idx >= 0 && persisted_home_idx < registry_count) {
        home_idx = persisted_home_idx;
    }
    atomic_set(&current_idx, home_idx);

    k_work_init(&nav_work, nav_work_cb);
    k_work_init(&idle_work, idle_work_cb);
    k_work_init(&splash_done_work, splash_done_work_cb);

    atomic_set(&last_activity_ms, (int32_t)(uint32_t)k_uptime_get());

    if (CONFIG_ZMK_DISPLAY_BOOT_SPLASH_MS > 0) {
        splash_page = zmk_display_boot_screen_create();
        splash_home_idx = home_idx;
        /* nav_apply() loads the home page (and frees the splash) when the
         * splash expires. */
        lv_screen_load(splash_page);
        k_timer_start(&splash_timer, K_MSEC(CONFIG_ZMK_DISPLAY_BOOT_SPLASH_MS), K_NO_WAIT);
    } else {
        atomic_set(&pending_nav, home_idx);
        k_work_submit_to_queue(zmk_display_work_q(), &nav_work);
    }

    /* The idle policy may run during the splash too: its shortest timeout
     * is minutes away, so it can only ever see IDLE_ACTIVE there. */
    k_timer_start(&idle_timer, K_MSEC(IDLE_POLICY_PERIOD_MS), K_MSEC(IDLE_POLICY_PERIOD_MS));

    return 0;
}
