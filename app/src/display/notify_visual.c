/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Visual notify backend (backlog a10). The only backend until h03/h04
 * put sound and light behind the BEEP/LIGHT channels; registered for
 * BOTH so a "beep" mode notification still lands visibly today. It
 * wakes the backlight (a08 policy) and fades a caption-sized line in
 * and out at the top of whichever page is up — the notify seam's
 * promise is that the current screen never has to know a notification
 * happened.
 *
 * notify backends run on the caller's thread (the pomodoro engine's
 * system-wq work), so everything LVGL here is posted to the display
 * work queue.
 */

#include <string.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <lvgl.h>

#include <zmk/display.h>
#include <zmk/display/screens.h>
#include <zmk/notify.h>

#include "styles.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define BANNER_MS 3000
#define FADE_MS 300 /* ease-in/out edges of the banner's life */

static lv_obj_t *banner;
static lv_timer_t *banner_timer;

static void banner_fade_exec(lv_obj_t *obj, int32_t v) {
    lv_obj_set_style_text_opa(obj, (lv_opa_t)v, 0);
}

static void banner_hide_timer_cb(lv_timer_t *timer) {
    ARG_UNUSED(timer);
    if (banner != NULL) {
        lv_obj_del(banner);
        banner = NULL;
    }
    banner_timer = NULL;
    lv_timer_del(timer);
}

static void banner_show_work_cb(struct k_work *work);
K_WORK_DEFINE(banner_work, banner_show_work_cb);

/* A plain shared slot would let two rapid notifications lose the first
 * text (the writer lands before the queued work runs); the msgq keeps
 * every kind until the display queue has shown/logged it. */
K_MSGQ_DEFINE(banner_msgq, sizeof(uint8_t), 4, 4);

static const char *banner_text(enum zmk_notify_kind kind) {
    switch (kind) {
    case ZMK_NOTIFY_POMO_WORK_DONE:
        return "break time";
    case ZMK_NOTIFY_POMO_REST_DONE:
        return "back to work";
    case ZMK_NOTIFY_POMO_SESSION_DONE:
        return "session complete";
    case ZMK_NOTIFY_POMO_CANCELLED:
        return "session cancelled";
    default:
        return zmk_notify_kind_name(kind);
    }
}

static void banner_show_work_cb(struct k_work *work) {
    lv_obj_t *scr = lv_screen_active();
    uint8_t kind_byte;
    enum zmk_notify_kind kind = ZMK_NOTIFY_KIND_COUNT; /* invalid until set */

    ARG_UNUSED(work);
    if (scr == NULL) {
        return;
    }

    /* Newest wins the banner; every queued kind gets its console line. */
    while (k_msgq_get(&banner_msgq, &kind_byte, K_NO_WAIT) == 0) {
        kind = (enum zmk_notify_kind)kind_byte;
    }
    if ((int)kind >= ZMK_NOTIFY_KIND_COUNT) {
        return; /* raced with itself: nothing pending */
    }

    if (banner != NULL) {
        lv_obj_del(banner);
    }
    if (banner_timer != NULL) {
        lv_timer_del(banner_timer);
        banner_timer = NULL;
    }

    /* Subtle by owner pass 2026-11-22: the first cut was a solid accent
     * pill with white text — a shout over the quiet pomodoro page. Now
     * a caption-sized, secondary-colour line that eases in and out; the
     * text_opa poke is animation state, not styling (font/colour stay
     * in the registry). The fades step at the LVGL handler rate (LVGL 9
     * has no per-anim period): ~10 full-frame flushes per fade over the
     * banner's 3 s life. */
    banner = lv_label_create(scr);
    lv_obj_add_style(banner, zmk_display_style(ZMK_STYLE_TEXT_CAPTION), 0);
    lv_label_set_text(banner, banner_text(kind));
    lv_obj_align(banner, LV_ALIGN_TOP_MID, 0, ZMK_GRID_UNIT);

    lv_anim_t fade;

    lv_anim_init(&fade);
    lv_anim_set_var(&fade, banner);
    lv_anim_set_exec_cb(&fade, banner_fade_exec);
    lv_anim_set_duration(&fade, FADE_MS);
    lv_anim_set_path_cb(&fade, lv_anim_path_ease_out);
    lv_anim_set_values(&fade, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_start(&fade);

    lv_anim_set_values(&fade, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_delay(&fade, BANNER_MS - FADE_MS);
    lv_anim_set_path_cb(&fade, lv_anim_path_ease_in);
    lv_anim_start(&fade);

    /* lv_timer runs on the display work queue like everything else LVGL
     * here; re-showing replaces the label and re-arms one timer. The
     * fades ride along (lv_obj_del removes anims bound to the label). */
    banner_timer = lv_timer_create(banner_hide_timer_cb, BANNER_MS, NULL);
}

static void visual_backend(enum zmk_notify_kind kind, enum zmk_notify_channel channel) {
    /* The visual answer is identical on both channels; log which one it
     * was so the h03/h04 backends can be A/B'd against it on console. */
    LOG_INF("notify %s via visual backend (%s channel)", zmk_notify_kind_name(kind),
            channel == ZMK_NOTIFY_CH_BEEP ? "beep" : "light");

    uint8_t kind_byte = (uint8_t)kind;

    if (k_msgq_put(&banner_msgq, &kind_byte, K_NO_WAIT) != 0) {
        LOG_WRN("notify banner queue full - showing newest only");
        (void)k_msgq_get(&banner_msgq, &kind_byte, K_NO_WAIT);
        (void)k_msgq_put(&banner_msgq, &kind, K_NO_WAIT);
    }
    zmk_display_backlight_wake();
    k_work_submit_to_queue(zmk_display_work_q(), &banner_work);
}

static int notify_visual_init(void) {
    zmk_notify_register_backend(ZMK_NOTIFY_CH_BEEP, visual_backend);
    zmk_notify_register_backend(ZMK_NOTIFY_CH_LIGHT, visual_backend);
    return 0;
}

/* After settings/display come up; registration alone is inert until a
 * zmk_notify() call, so ordering only matters against the first session. */
SYS_INIT(notify_visual_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
