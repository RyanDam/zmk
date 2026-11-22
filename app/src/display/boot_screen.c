/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Boot splash page for the screen manager (backlog a08) — the equivalent of
 * pad10a's graphics/screens/screen_boot.c, shown first and replaced by the
 * persisted home page after CONFIG_ZMK_DISPLAY_BOOT_SPLASH_MS. It gives the
 * first full-frame flush of the panel a quiet moment to land and tells the
 * owner the firmware is alive before any real screen exists.
 *
 * Styled only through the design system registry (a07 rule).
 */

#include <lvgl.h>

#include "anim.h"
#include "styles.h"

lv_obj_t *zmk_display_boot_screen_create(void) {
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_add_style(screen, zmk_display_style(ZMK_STYLE_SCREEN), 0);

    /* a12: boot animation first child = behind the splash labels */
#if IS_ENABLED(CONFIG_ZMK_DISPLAY_ANIM)
    if (zmk_anim_available("boot")) {
        if (zmk_anim_create(screen, "boot") == NULL) {
            /* keep the static splash; logged once by the store/player */
        }
    }
#endif

    lv_obj_t *title = lv_label_create(screen);
    lv_obj_add_style(title, zmk_display_style(ZMK_STYLE_TEXT_HERO), 0);
    lv_label_set_text(title, "Coban Tab");
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -ZMK_GRID_UNIT * 2);

    lv_obj_t *sub = lv_label_create(screen);
    lv_obj_add_style(sub, zmk_display_style(ZMK_STYLE_TEXT_CAPTION), 0);
    lv_label_set_text(sub, "starting up");
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, ZMK_GRID_UNIT * 6);

    return screen;
}
