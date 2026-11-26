/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Standalone animation page: full-bleed playback of the store's GIFs, "animbg"
 * first and a tap cycling the rest. Unlike the other pages' backgrounds it
 * plays even when the anim-bg setting is off. Playback and lifecycle
 * (auto-pause while hidden, resume on return) live in anim.c and the pixels in
 * the asset store, so a host upload appears here with no page change. The
 * stock GIF is bench fan-art and must not ship; the page is product code.
 */

#include <string.h>

#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <lvgl.h>

#include <zmk/assets.h>
#include <zmk/display/screens.h>

#include "anim.h"
#include "styles.h"
#include "ui_helpers.h"

LOG_MODULE_REGISTER(display_anim_screen, CONFIG_ZMK_LOG_LEVEL);

#define PAGE_NAME "anim"

static lv_obj_t *screen_root;

/* Logical index into the store's GIF-only subset (the store also
 * holds Lottie/dev assets); -1 until the first pick. */
static int cur_gif = -1;

static int gif_total(void) {
    int n = 0;

    for (int i = 0; i < zmk_asset_count(); i++) {
        if (zmk_asset_get(i)->kind == ZMK_ASSET_GIF) {
            n++;
        }
    }
    return n;
}

static const struct zmk_asset_info *gif_at(int idx) {
    int seen = 0;

    for (int i = 0; i < zmk_asset_count(); i++) {
        if (zmk_asset_get(i)->kind == ZMK_ASSET_GIF) {
            if (seen++ == idx) {
                return zmk_asset_get(i);
            }
        }
    }
    return NULL;
}

static void anim_rebuild(void) {
    int total = gif_total();

    if (screen_root == NULL) {
        return;
    }

    lv_obj_clean(screen_root);

    if (cur_gif < 0 || cur_gif >= total) {
        /* First build or store shrank under us: prefer the active asset
         * (factory default "animbg"). */
        const struct zmk_asset_info *bg = zmk_asset_find(zmk_anim_active_get());
        int pick = 0;

        for (int i = 0; i < total; i++) {
            if (gif_at(i) == bg) {
                pick = i;
                break;
            }
        }
        cur_gif = total > 0 ? pick : -1;
    }

    if (cur_gif < 0) {
        lv_obj_t *hint = zmk_ui_label(screen_root, ZMK_STYLE_TEXT_SECONDARY);

        lv_label_set_text(hint, "no animation assets");
        lv_obj_center(hint);
        return;
    }

    if (zmk_anim_page_attach(screen_root, PAGE_NAME, gif_at(cur_gif)->name) == NULL) {
        LOG_WRN("anim page: '%s' failed to attach", gif_at(cur_gif)->name);
    }
}

ZMK_UI_WORK_DEFINE(anim_tap_work, anim_tap_work_cb) {
    ARG_UNUSED(work);

    if (gif_total() > 1) {
        cur_gif = (cur_gif + 1) % gif_total();
    }
    anim_rebuild();
}

static lv_obj_t *anim_page_create(lv_obj_t *parent) {
    screen_root = lv_obj_create(parent);
    lv_obj_remove_style_all(screen_root);
    lv_obj_set_size(screen_root, lv_pct(100), lv_pct(100));

    anim_rebuild();

    return parent;
}

ZMK_DISPLAY_DEFINE_SCREEN(anim, PAGE_NAME, anim_page_create, 50);

/* Tap cycles the GIF; the shared detector leaves swipes (>= 64 px) to
 * the screen manager (same contract as the other pages). */
ZMK_UI_TAP_STATE(anim_tap) = {.page_name = PAGE_NAME, .work = &anim_tap_work};

static void anim_touch_cb(struct input_event *evt, void *user_data) {
    ARG_UNUSED(user_data);
    zmk_ui_tap_input(&anim_tap, evt);
}
INPUT_CALLBACK_DEFINE(NULL, anim_touch_cb, NULL);
