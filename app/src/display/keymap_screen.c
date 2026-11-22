/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Virtual key matrix screen for the Coban Tab 1a (backlog a13).
 *
 * The pad10a layer screen (graphics/screens/screen_layers.c) drew the
 * keymap read-only with style_key tiles; this page goes one step further
 * and is the input: tapping a tile fires the cell's ZMK binding through
 * the vkeymap model, exactly like a hardware key would. The screen only
 * ever talks to <zmk/vkeymap.h> — the acceptance grep for the seam.
 *
 * Touch here is pure LVGL (the a13 indev feeds PRESSED/CLICKED), unlike
 * the tap-detector pages: press-flash needs the down edge, and clicks
 * inside a tile never look like a swipe to the manager.
 *
 * Soft layers are a model concept (grid contents); the footer also shows
 * the real keymap layer so the two can be watched side by side until
 * h04's hardware keys and a14's editing make that distinction richer.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <lvgl.h>

#include <zmk/display.h>
#include <zmk/display/screens.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/keymap.h>
#include <zmk/vkeymap.h>

#include "styles.h"
#include "ui_helpers.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define PAGE_NAME "keymap"

#define TILE_SIZE (ZMK_GRID_UNIT * 8) /* 64 px, > touch min     */
#define TILE_GAP ZMK_GRID_UNIT        /* 8 px                   */
#define GRID_W (ZMK_VKEYMAP_COLS * TILE_SIZE + (ZMK_VKEYMAP_COLS - 1) * TILE_GAP)
#define GRID_X ((ZMK_GRID_SCREEN_W - GRID_W) / 2)
#define GRID_Y (ZMK_GRID_UNIT * 15) /* below title + layer chips */

static lv_obj_t *screen_root;
static lv_obj_t *tiles[ZMK_VKEYMAP_CELLS];
static lv_obj_t *tile_labels[ZMK_VKEYMAP_CELLS];
static lv_obj_t *chips[ZMK_VKEYMAP_LAYERS];
static lv_obj_t *kbd_layer_label;
static unsigned int soft_layer;
static char tile_cache[ZMK_VKEYMAP_CELLS][16];
static lv_opa_t tile_opa_cache[ZMK_VKEYMAP_CELLS];

static void tile_labels_refresh(void);

/* ------------------------------------------------------------------ */
/* Fire + reload work (display work queue owns all LVGL touching)      */
/* ------------------------------------------------------------------ */

static unsigned int fire_idx;

ZMK_UI_WORK_DEFINE(reload_work, reload_work_cb) {
    ARG_UNUSED(work);
    tile_labels_refresh();
    for (unsigned int l = 0; l < ZMK_VKEYMAP_LAYERS && l < ARRAY_SIZE(chips); l++) {
        if (chips[l] != NULL) {
            lv_label_set_text(lv_obj_get_child(chips[l], 0), zmk_vkeymap_layer_name(l));
        }
    }
}

ZMK_UI_WORK_DEFINE(fire_work, fire_work_cb) {
    ARG_UNUSED(work);
    int err = zmk_vkeymap_fire(soft_layer, fire_idx);

    if (err && err != -EINVAL) {
        LOG_WRN("keymap screen fire L%u C%u failed (err %d)", soft_layer, fire_idx, err);
    }
}

/* vkeymap model changed (shell, settings reload, a14): redraw labels. */
static void vkeymap_changed_cb(void) { zmk_ui_post(&reload_work); }

/* ------------------------------------------------------------------ */
/* Tiles                                                               */
/* ------------------------------------------------------------------ */

static void tile_event_cb(lv_event_t *e) {
    lv_obj_t *tile = lv_event_get_target(e);
    lv_event_code_t code = lv_event_get_code(e);
    unsigned int idx = (unsigned int)(uintptr_t)lv_event_get_user_data(e);

    switch (code) {
    case LV_EVENT_PRESSED:
        lv_obj_add_style(tile, zmk_display_style(ZMK_STYLE_TOUCH_PRESSED), 0);
        break;
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
        lv_obj_remove_style(tile, zmk_display_style(ZMK_STYLE_TOUCH_PRESSED), 0);
        break;
    case LV_EVENT_CLICKED:
        fire_idx = idx;
        zmk_ui_post(&fire_work);
        break;
    default:
        break;
    }
}

static void tile_labels_refresh(void) {
    for (unsigned int i = 0; i < ZMK_VKEYMAP_CELLS; i++) {
        const struct zmk_vkeymap_cell *cell = zmk_vkeymap_cell_get(soft_layer, i);
        const char *text;
        lv_opa_t opa;

        if (tile_labels[i] == NULL || cell == NULL) {
            continue;
        }
        text = cell->name[0] ? cell->name : (cell->behavior[0] ? "?" : "—");
        /* cached: a reload that changes nothing must not invalidate */
        zmk_ui_set_text(tile_cache[i], sizeof(tile_cache[i]), tile_labels[i], text);

        /* unassigned tiles sit visually dead */
        opa = cell->behavior[0] ? LV_OPA_COVER : LV_OPA_30;
        if (tile_opa_cache[i] != opa) {
            lv_obj_set_style_bg_opa(tiles[i], opa, 0);
            tile_opa_cache[i] = opa;
        }
    }
}

static void grid_build(void) {
    for (unsigned int i = 0; i < ZMK_VKEYMAP_CELLS; i++) {
        unsigned int row = i / ZMK_VKEYMAP_COLS;
        unsigned int col = i % ZMK_VKEYMAP_COLS;

        lv_obj_t *tile = lv_obj_create(screen_root);

        lv_obj_remove_style_all(tile);
        lv_obj_add_style(tile, zmk_display_style(ZMK_STYLE_TOUCH), 0);
        lv_obj_set_size(tile, TILE_SIZE, TILE_SIZE);
        lv_obj_set_pos(tile, GRID_X + col * (TILE_SIZE + TILE_GAP),
                       GRID_Y + row * (TILE_SIZE + TILE_GAP));
        lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
        /* one dispatcher for all events (review pass: 4 registrations per
         * tile were 64 event objects for the same function) */
        lv_obj_add_event_cb(tile, tile_event_cb, LV_EVENT_ALL, (void *)(uintptr_t)i);
        tiles[i] = tile;

        lv_obj_t *lbl = zmk_ui_label(tile, ZMK_STYLE_TEXT_BODY);

        lv_obj_center(lbl);
        tile_labels[i] = lbl;
    }
    tile_labels_refresh();
}

/* ------------------------------------------------------------------ */
/* Soft-layer chips                                                    */
/* ------------------------------------------------------------------ */

static void chip_event_cb(lv_event_t *e) {
    unsigned int layer = (unsigned int)(uintptr_t)lv_event_get_user_data(e);

    if (layer == soft_layer) {
        return;
    }
    soft_layer = layer;
    LOG_INF("keymap screen soft layer -> %u (%s)", layer, zmk_vkeymap_layer_name(layer));

    for (unsigned int l = 0; l < ZMK_VKEYMAP_LAYERS && l < ARRAY_SIZE(chips); l++) {
        if (chips[l] == NULL) {
            continue;
        }
        zmk_ui_style_toggle(chips[l], zmk_display_style(ZMK_STYLE_ACCENT), l == soft_layer);
    }
    tile_labels_refresh();
}

static void chips_build(void) {
    lv_obj_t *row = zmk_ui_flex(screen_root, LV_FLEX_FLOW_ROW, TILE_GAP);

    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, ZMK_GRID_MARGIN + ZMK_GRID_UNIT * 5);

    for (unsigned int l = 0; l < ZMK_VKEYMAP_LAYERS; l++) {
        lv_obj_t *chip = lv_obj_create(row);

        lv_obj_remove_style_all(chip);
        lv_obj_add_style(chip, zmk_display_style(ZMK_STYLE_TOUCH), 0);
        lv_obj_set_size(chip, LV_SIZE_CONTENT, ZMK_GRID_TOUCH_MIN);
        lv_obj_set_style_pad_hor(chip, ZMK_GRID_UNIT * 2, 0);
        lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(chip, chip_event_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)l);

        lv_obj_t *lbl = zmk_ui_label(chip, ZMK_STYLE_TEXT_CAPTION);

        lv_label_set_text(lbl, zmk_vkeymap_layer_name(l));
        lv_obj_center(lbl);
        chips[l] = chip;
    }
    /* active soft layer reads accent-filled (registry style composition) */
    lv_obj_add_style(chips[soft_layer], zmk_display_style(ZMK_STYLE_ACCENT), 0);
}

/* ------------------------------------------------------------------ */
/* Real keymap layer highlight (a13 acceptance)                        */
/* ------------------------------------------------------------------ */

static char kbd_layer_cache[40];

static void kbd_layer_update(void) {
    zmk_keymap_layer_index_t idx = zmk_keymap_highest_layer_active();
    const char *name = zmk_keymap_layer_name(zmk_keymap_layer_index_to_id(idx));
    char buf[40];

    if (kbd_layer_label == NULL) {
        return;
    }
    if (name != NULL) {
        snprintk(buf, sizeof(buf), "kbd layer: %s", name);
    } else {
        snprintk(buf, sizeof(buf), "kbd layer: %u", (unsigned int)idx);
    }
    zmk_ui_set_text(kbd_layer_cache, sizeof(kbd_layer_cache), kbd_layer_label, buf);
}

ZMK_UI_WORK_DEFINE(kbd_layer_work, kbd_layer_work_cb) {
    ARG_UNUSED(work);
    kbd_layer_update();
}

static int layer_changed_event_cb(const zmk_event_t *eh) {
    ARG_UNUSED(eh);
    if (kbd_layer_label != NULL) {
        zmk_ui_post(&kbd_layer_work);
    }
    return 0;
}

ZMK_LISTENER(keymap_screen, layer_changed_event_cb);
ZMK_SUBSCRIPTION(keymap_screen, zmk_layer_state_changed);

/* ------------------------------------------------------------------ */
/* Reload + build                                                      */
/* ------------------------------------------------------------------ */

static lv_obj_t *keymap_page_create(lv_obj_t *parent) {
    screen_root = lv_obj_create(parent);
    lv_obj_remove_style_all(screen_root);
    lv_obj_set_size(screen_root, lv_pct(100), lv_pct(100));

    lv_obj_t *title = zmk_ui_label(screen_root, ZMK_STYLE_TEXT_TITLE);

    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, ZMK_GRID_MARGIN + ZMK_GRID_UNIT);
    lv_label_set_text(title, "Keys");

    chips_build();
    grid_build();

    kbd_layer_label = zmk_ui_label(screen_root, ZMK_STYLE_TEXT_CAPTION);
    lv_obj_align(kbd_layer_label, LV_ALIGN_BOTTOM_MID, 0, -ZMK_GRID_MARGIN * 4);
    kbd_layer_update();

    lv_obj_t *hint = zmk_ui_label(screen_root, ZMK_STYLE_TEXT_CAPTION);

    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -ZMK_GRID_MARGIN * 2);
    lv_label_set_text(hint, "tap a key to fire it");

    zmk_vkeymap_add_listener(vkeymap_changed_cb);
    return parent;
}

ZMK_DISPLAY_DEFINE_SCREEN(keymap, PAGE_NAME, keymap_page_create, 40);
