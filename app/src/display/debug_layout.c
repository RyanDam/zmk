/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Layout debug borders (bench harness, complements A06's touch readout):
 * draws a colored outline on every plain LVGL container so page boxes, the
 * reach band and flex padding are visible on the glass without tooling.
 * Only compiled in with CONFIG_ZMK_DISPLAY_DEBUG_LAYOUT (off in product
 * builds); backlog d05 may fold it under one debug-overlay toggle later.
 */

#include <zephyr/sys/util.h>

#include <lvgl.h>

#include "debug_layout.h"

/*
 * Only exact plain containers (lv_obj_class) get a box: lv_obj_check_type
 * is an exact class match in LVGL 9.5 (lv_obj.c:428), so labels, arcs and
 * scrollbars keep their own look. The border is a LOCAL style, which wins
 * over the registry styles that set border width 0 (styles.c), so a page
 * cannot accidentally hide its own debug box; borders draw inside the
 * object box, so nothing moves while layout debugging.
 */
static void debug_border_apply(lv_obj_t *obj) {
    if (!lv_obj_check_type(obj, &lv_obj_class)) {
        return;
    }
    lv_obj_set_style_border_width(obj, CONFIG_ZMK_DISPLAY_DEBUG_LAYOUT_PX, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(CONFIG_ZMK_DISPLAY_DEBUG_LAYOUT_COLOR), 0);
    lv_obj_set_style_border_opa(obj, LV_OPA_COVER, 0);
}

static void debug_border_tree(lv_obj_t *root) {
    for (uint32_t i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *child = lv_obj_get_child(root, i);

        debug_border_apply(child);
        debug_border_tree(child);
    }
}

/*
 * LV_EVENT_CHILD_CREATED always bubbles to every ancestor (LVGL 9.5,
 * lv_obj_event.c event_is_bubbled), so one callback on the page screen
 * catches every container created later — runtime widget rebuilds (keymap
 * tiles, stats layouts, page dots) included. The param is the new child
 * (lv_obj_class.c:129); the reparent path sends NULL (lv_obj_tree.c:201).
 */
static void debug_child_created_cb(lv_event_t *e) {
    lv_obj_t *child = lv_event_get_param(e);

    if (child == NULL) {
        return;
    }
    debug_border_apply(child);
    debug_border_tree(child);
}

void zmk_display_debug_layout_watch(lv_obj_t *screen) {
    if (screen == NULL) {
        return;
    }

    debug_border_tree(screen);
    lv_obj_add_event_cb(screen, debug_child_created_cb, LV_EVENT_CHILD_CREATED, NULL);
}
