/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Key matrix screen for the Coban Tab 1a (backlog a13, redesigned after the
 * owner audit 2026-11-23).
 *
 * The tab's keys are REAL keymap positions (zmk-config cobantab1a.keymap,
 * 4 rows x 4 cols): row 0 (positions 0..3) is the four PHYSICAL keys
 * (h04) — never drawn — and this page renders the other 12, the virtual
 * grid (positions 4..15), like pad10a's layer screen
 * (graphics/screens/screen_layers.c) draws its dynamic keymap. Touching
 * a tile feeds the position through the exact position-state path a
 * scannable matrix uses — raise_zmk_position_state_changed() on
 * press and on release, timestamps taken at the touch edges — so press,
 * hold, tap-hold, layermasks and macros behave exactly like a physical
 * key. Studio edits these same positions with its physical-key RPC; the
 * tile flash follows the real key events (pad10a layers_cb pattern), never
 * raw touch, so h04 hardware keys and Studio edits light the grid too.
 *
 * Frame parity with pad10a: layer indicator squares (style_indice), tiles
 * that read as quiet 20 % fills and light to 100 % while the KEY is down
 * (style_key / layers_cb:139-160). ALL drawn cells are plain keys — unlike
 * pad10a, there are no encoder seats and no special-cased cells (owner
 * pass 5): every tile fires its binding. Layer switching is the
 * indicator's job: the pips at the bottom are buttons (tap pip n ->
 * layer n), and the physical row's keys carry per-layer `&to` prev/next
 * bindings, so the screen only ever shows the layer the device is
 * actually on. Only MACROS carry user-visible names; layers are plain
 * numbered pips (owner pass 3).
 *
 * The old parallel "vkeymap" model (its own layers/cells/settings blob)
 * is deleted: one matrix, one source of truth.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <lvgl.h>

#include <zmk/behavior_dynamic_macro.h>
#include <zmk/display.h>
#include <zmk/display/screens.h>
#include <zmk/events/keymap_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/keymap.h>
#include <zmk/matrix.h>

#include <dt-bindings/zmk/hid_usage.h>
#include <dt-bindings/zmk/hid_usage_pages.h>

#include "styles.h"
#include "ui_helpers.h"
#if IS_ENABLED(CONFIG_ZMK_DISPLAY_ANIM)
#include "anim.h"
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define PAGE_NAME "keymap"

/* Keep in step with the zmk-config cobantab1a matrix geometry: keymap row
 * 0 (positions 0..3) is the four PHYSICAL keys (h04) — not drawn here —
 * and the drawn 3x4 grid is positions 4..15. */
#define ZMK_PHYS_ROW 4
#define ZMK_VKEY_ROWS 3
#define ZMK_VKEY_COLS 4
#define ZMK_VKEY_CELLS (ZMK_VKEY_ROWS * ZMK_VKEY_COLS)
BUILD_ASSERT(ZMK_PHYS_ROW + ZMK_VKEY_CELLS == ZMK_KEYMAP_LEN,
             "screen grid + physical row must match the keymap size");

#define TILE_GAP ZMK_GRID_UNIT                             /* 8 px  */
#define TILE_W                                               \
    ((ZMK_GRID_SCREEN_W - ZMK_GRID_UNIT * 3 -               \
      (ZMK_VKEY_COLS - 1) * TILE_GAP) / ZMK_VKEY_COLS)       /* 108  */
#define TILE_H (TILE_W - ZMK_GRID_UNIT * 4)                 /* 76    */
#define GRID_X                                                \
    ((ZMK_GRID_SCREEN_W -                                     \
      (ZMK_VKEY_COLS * TILE_W + (ZMK_VKEY_COLS - 1) * TILE_GAP)) / 2)
#define GRID_Y ZMK_GRID_UNIT /* keys to the top (owner pass 5)    */
#define BAND_H (ZMK_GRID_UNIT * 7) /* indicator band height        */
#define GRID_BOT (GRID_Y + ZMK_VKEY_ROWS * TILE_H + (ZMK_VKEY_ROWS - 1) * TILE_GAP)
BUILD_ASSERT(GRID_BOT + ZMK_GRID_UNIT + BAND_H <= ZMK_GRID_SCREEN_H,
             "grid + indicator band must fit the screen");

static lv_obj_t *screen_root;
static lv_obj_t *tiles[ZMK_VKEY_CELLS];
static lv_obj_t *tile_labels[ZMK_VKEY_CELLS];
static char tile_cache[ZMK_VKEY_CELLS][16];
static bool tile_lit[ZMK_VKEY_CELLS];
static lv_obj_t *dots_row;
static lv_obj_t *dots[ZMK_KEYMAP_LAYERS_LEN];
static lv_obj_t *dots_num[ZMK_KEYMAP_LAYERS_LEN];
static bool dot_active_cache[ZMK_KEYMAP_LAYERS_LEN];
static unsigned int dot_count;

/* ------------------------------------------------------------------ */
/* Firing: touch edges -> real key positions                            */
/* ------------------------------------------------------------------ */

/* Ring so two fast taps can't rebind a pending edge; the timestamp is
 * captured at the touch edge (LVGL thread), executed on the display work
 * queue like every other page action — same work-queue context a kscan
 * driver raises from. */
struct key_edge {
    uint32_t position;
    bool pressed;
    int64_t timestamp;
};

#define EDGE_RING 8
static struct key_edge edges[EDGE_RING];
static uint8_t edges_in, edges_out;

/* Per-position held state: post_edge mirrors the kscan contract (edges
 * only on real transitions). Without it LVGL's PRESS_LOST + RELEASED
 * pair for one drag-tap fires TWO releases, and ghost capacitive hits
 * can fire presses that never belonged — unpaired edges corrupt the
 * keymap's per-position layer state and the HID reports (stuck keys). */
static bool edge_held[ZMK_PHYS_ROW + ZMK_VKEY_CELLS];

ZMK_UI_WORK_DEFINE(edge_work, edge_work_cb) {
    ARG_UNUSED(work);
    while (edges_out != edges_in) {
        struct key_edge *e = &edges[edges_out];

        edges_out = (edges_out + 1) % EDGE_RING;

        struct zmk_position_state_changed ev = {
            .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
            .position = e->position,
            .state = e->pressed,
            .timestamp = e->timestamp,
        };

        int err = raise_zmk_position_state_changed(ev);

        if (err) {
            LOG_WRN("keymap screen edge raise failed (err %d)", err);
        }
    }
}

static void post_edge(uint32_t position, bool pressed) {
    struct key_edge *slot;

    if (position >= ARRAY_SIZE(edge_held) || edge_held[position] == pressed) {
        return;
    }
    edge_held[position] = pressed;

    slot = &edges[edges_in];
    slot->position = position;
    slot->pressed = pressed;
    slot->timestamp = k_uptime_get();
    edges_in = (edges_in + 1) % EDGE_RING;

    zmk_ui_post(&edge_work);
}

/* ------------------------------------------------------------------ */
/* Layer switching (the indicator pips fire this)                       */
/* ------------------------------------------------------------------ */

#define NAV_IDLE 0
#define NAV_GOTO 1 /* jump straight to nav_goto's layer index (pip tap) */
static volatile uint8_t nav_cmd;
static volatile uint8_t nav_goto;

static unsigned int layer_count(void);

/* Layer switching is the indicator pips' job (tap pip n -> layer n): the
 * grid's 12 tiles are all plain keys (owner pass 5 — pad10a's encoder
 * seats are NOT special here). */

ZMK_UI_WORK_DEFINE(nav_work, nav_work_cb) {
    ARG_UNUSED(work);

    if (nav_cmd != NAV_GOTO) {
        return;
    }

    unsigned int target = nav_goto;
    zmk_keymap_layer_id_t id = zmk_keymap_layer_index_to_id((zmk_keymap_layer_index_t)target);

    nav_cmd = NAV_IDLE;
    if (id == ZMK_KEYMAP_LAYER_ID_INVAL) {
        return;
    }

    LOG_INF("keymap screen layer -> %u", target);
    zmk_keymap_layer_to(id, false);
}

/* ------------------------------------------------------------------ */
/* Tile labels from the keymap                                          */
/* ------------------------------------------------------------------ */

#define ICON_VOL_DOWN "\xEF\x80\xA7" /* f027 volume-down       */
#define ICON_VOL_UP   "\xEF\x80\xA8" /* f028 volume-up         */
#define ICON_PREV     "\xEF\x81\x8A" /* f04a backward          */
#define ICON_PLAY     "\xEF\x81\x8B" /* f04b play              */
#define ICON_PAUSE    "\xEF\x81\x8C" /* f04c pause             */
#define ICON_STOP     "\xEF\x81\x8D" /* f04d stop              */
#define ICON_NEXT     "\xEF\x81\x8E" /* f04e forward           */
#define ICON_LEFT     "\xEF\x81\xA0" /* f060 arrow-left        */
#define ICON_RIGHT    "\xEF\x81\xA1" /* f061 arrow-right       */
#define ICON_UP       "\xEF\x81\xA2" /* f062 arrow-up          */
#define ICON_DOWN     "\xEF\x81\xA3" /* f063 arrow-down        */
#define ICON_MUTE     "\xEF\x9A\xA9" /* f6a9 volume-mute       */
#define ICON_CARET_DOWN "\xEF\x83\x97" /* f0d7 caret-down: transparent key
                                        * (pad10a: {CB_ICON_CARET_DOWN, KC_TRNS}) */

/* Compact labels for the behaviors a tab1a cell commonly carries. The
 * device-name strings are the behavior nodes' devicetree node names
 * (behaviors.dtsi) — what zmk_behavior_get_binding() matches; bindings
 * with richer meaning (&mt/&lt/&sk, bt, outputs, macros) are
 * special-cased in binding_label(). Unknown behaviors fall back to the
 * device name, truncated: never a wrong lie, occasionally verbose. */
static struct {
    const char *dev;
    const char *fmt; /* %lu = param1 */
} DEV_LABELS[] = {
    {"momentary_layer", "MO %lu"},
    {"to_layer", "TO %lu"},
    {"toggle_layer", "TG %lu"},
    {"key_toggle", "KT %lu"},
    {"transparent", ICON_CARET_DOWN}, /* inherits the layer below (pad10a
                                       * caret-down, owner pass 5) */
    {"key_repeat", "REPT"},
    {"caps_word", "CAPSW"},
    {"macro", "MAC %lu"},
    {"backlight", "BL %lu"},
    {"rgb_underglow", "RGB %lu"},
    {"ext_power", "PWR"},
    {"soft_off", "SOFF"},
    {"studio_unlock", "STUD"},
    {"bootload", "BOOT"},
    {"sysreset", "RST"},
    {"mouse_key_press", "MBTN"},
    {"mouse_move", "MS"},
    {"mouse_scroll", "WHL"},
    {"enc_key_press", "ENC"},
    {"gresc", "GESC"},
};

/* The screen's answer to pad10a's utils/keycode_lookup.c: every binding
 * a tab cell can carry gets a short on-glass label. Text is ASCII (the
 * barlow ramps), icons are FontAwesome5 codepoints (LVGL docs
 * main-modules/fonts/overview#symbols) that render through barlow_24's
 * `fallback` = lv_font_tab1a_icons_24 — the same trick pad10a's barlow.c
 * does with fonts/icons.c. Glyph ids mirror pad10a's CB_ICON_* defines
 * (keycode_lookup.c:23-35) so both screens speak one icon language.
 * Unmapped usages render as "?U4C"-style hex — a visible TODO, not a
 * silent "?". Raw usage IDs follow the HID Usage Tables. */
static const struct {
    uint16_t usage;
    const char *label;
} KB_USAGE_LABELS[] = {
    {0x28, "ENT"},
    {0x29, "ESC"},  {0x2A, "BSP"}, {0x2B, "TAB"}, {0x2C, "SPC"},
    {0x2D, "-"},    {0x2E, "="},   {0x2F, "["},   {0x30, "]"},
    {0x31, "\\"},   {0x32, "#"}, /* NONUS_HASH */
    {0x33, ";"},    {0x34, "'"},   {0x35, "`"},   {0x36, ","},
    {0x37, "."},    {0x38, "/"},   {0x39, "CAPS"},
    {0x46, "PRT"},  {0x47, "SCR"}, {0x48, "PAU"}, {0x49, "INS"},
    {0x4A, "HOME"}, {0x4B, "Pg " ICON_UP}, {0x4C, "DEL"}, {0x4D, "END"},
    {0x4E, "Pg " ICON_DOWN},
    {0x4F, ICON_RIGHT}, {0x50, ICON_LEFT}, {0x51, ICON_DOWN}, {0x52, ICON_UP},
    {0x53, "NML"}, /* NUM_LOCK */
    {0x54, "K/"},   {0x55, "K*"},  {0x56, "K-"},  {0x57, "K+"},
    {0x58, "KENT"},
    {0x59, "K1"},   {0x5A, "K2"},  {0x5B, "K3"},  {0x5C, "K4"},
    {0x5D, "K5"},   {0x5E, "K6"},  {0x5F, "K7"},  {0x60, "K8"},
    {0x61, "K9"},   {0x62, "K0"},  {0x63, "K."},
    {0x64, "|"}, /* NONUS_BACKSLASH */
    {0x65, "APP"}, {0x67, "K="},
};

static const char *kb_sparse_label(uint16_t usage) {
    for (size_t i = 0; i < ARRAY_SIZE(KB_USAGE_LABELS); i++) {
        if (KB_USAGE_LABELS[i].usage == usage) {
            return KB_USAGE_LABELS[i].label;
        }
    }
    return NULL;
}

/* "CSAG"-style prefix from the &kp param2 / mod-tap modifier flags
 * (the zmk/keys.h mod bits; L/R collapsed — the tile shows intent, not
 * which thumb hits it). Empty string when nothing is held. */
static void mods_prefix(char *p, size_t n, uint32_t mods) {
    size_t len = 0;

    if (mods & 0x11) {
        p[len++] = 'C'; /* LCTL|RCTL */
    }
    if (mods & 0x22) {
        p[len++] = 'S'; /* LSFT|RSFT */
    }
    if (mods & 0x44) {
        p[len++] = 'A'; /* LALT|RALT */
    }
    if (mods & 0x88) {
        p[len++] = 'G'; /* LGUI|RGUI */
    }
    p[len < n ? len : n - 1] = '\0';
}

/* Full ZMK keycode ((page << 16) | usage) -> shortest honest label. */
static void keycode_base(char *base, size_t n, uint32_t keycode) {
    uint32_t page = ZMK_HID_USAGE_PAGE(keycode);
    uint16_t usage = ZMK_HID_USAGE_ID(keycode);

    if (page == HID_USAGE_KEY) {
        if (usage >= HID_USAGE_KEY_KEYBOARD_A && usage <= HID_USAGE_KEY_KEYBOARD_Z) {
            snprintk(base, n, "%c", (char)('A' + (usage - HID_USAGE_KEY_KEYBOARD_A)));
        } else if (usage >= 0x1E && usage <= 0x27) { /* 1..9,0 (HID usages) */
            snprintk(base, n, "%c", usage == 0x27 ? '0' : (char)('1' + (usage - 0x1E)));
        } else if (usage >= 0x3A && usage <= 0x45) { /* F1..F12 */
            snprintk(base, n, "F%lu", usage - 0x3A + 1);
        } else if (usage >= 0x68 && usage <= 0x73) { /* F13..F24 */
            snprintk(base, n, "F%lu", usage - 0x68 + 13);
        } else if (usage >= 0xE0 && usage <= 0xE7) { /* mods: LCTL..LGUI, RCTL..RGUI */
            static const char *const mod_usage[8] = {"LC", "LS", "LA", "LG",
                                                     "RC", "RS", "RA", "RG"};
            snprintk(base, n, "%s", mod_usage[usage - 0xE0]);
        } else {
            const char *sparse = kb_sparse_label(usage);

            if (sparse != NULL) {
                strcpy(base, sparse);
            } else {
                snprintk(base, n, "?U%02X", usage);
            }
        }
    } else if (page == HID_USAGE_CONSUMER) {
        switch (usage) {
        case HID_USAGE_CONSUMER_PLAY_PAUSE:
            strcpy(base, ICON_PLAY);
            break;
        case HID_USAGE_CONSUMER_SCAN_NEXT_TRACK:
            strcpy(base, ICON_NEXT);
            break;
        case HID_USAGE_CONSUMER_SCAN_PREVIOUS_TRACK:
            strcpy(base, ICON_PREV);
            break;
        case HID_USAGE_CONSUMER_STOP:
            strcpy(base, ICON_STOP);
            break;
        case HID_USAGE_CONSUMER_MUTE:
            strcpy(base, ICON_MUTE);
            break;
        case HID_USAGE_CONSUMER_VOLUME_INCREMENT:
            strcpy(base, ICON_VOL_UP);
            break;
        case HID_USAGE_CONSUMER_VOLUME_DECREMENT:
            strcpy(base, ICON_VOL_DOWN);
            break;
        case HID_USAGE_CONSUMER_RECORD:
            strcpy(base, "REC");
            break;
        case HID_USAGE_CONSUMER_FAST_FORWARD:
            strcpy(base, "FF");
            break;
        case HID_USAGE_CONSUMER_REWIND:
            strcpy(base, "REW");
            break;
        case HID_USAGE_CONSUMER_STOP_EJECT:
            strcpy(base, "EJT");
            break;
        case HID_USAGE_CONSUMER_DISPLAY_BRIGHTNESS_INCREMENT:
            strcpy(base, "BRT+");
            break;
        case HID_USAGE_CONSUMER_DISPLAY_BRIGHTNESS_DECREMENT:
            strcpy(base, "BRT-");
            break;
        default:
            snprintk(base, n, "?C%02X", usage);
            break;
        }
    } else if (page == HID_USAGE_GD) {
        switch (usage) {
        case HID_USAGE_GD_SYSTEM_POWER_DOWN:
            strcpy(base, "PWR");
            break;
        case HID_USAGE_GD_SYSTEM_SLEEP:
            strcpy(base, "SLP");
            break;
        case HID_USAGE_GD_SYSTEM_WAKE_UP:
            strcpy(base, "WAKE");
            break;
        default:
            snprintk(base, n, "?G%02X", usage);
            break;
        }
    } else {
        snprintk(base, n, "?%lX.%02X", (unsigned long)page, usage);
    }
}

/* &kp binding (param1 = full keycode, param2 = mod flags) and any plain
 * keycode caller: "S/ENT", "CG/K", "PLAY" ... */
static void key_press_label(char *out, size_t n, uint32_t keycode, uint32_t mods) {
    char base[8];
    char pre[5];

    keycode_base(base, sizeof(base), keycode);
    mods_prefix(pre, sizeof(pre), mods);

    if (pre[0] != '\0') {
        snprintk(out, n, "%s/%s", pre, base);
    } else {
        snprintk(out, n, "%s", base);
    }
}

static void binding_label(uint32_t position, zmk_keymap_layer_id_t layer, char *out, size_t n) {
    const struct zmk_behavior_binding *b = zmk_keymap_get_layer_binding_at_idx(layer, position);

    if (b == NULL || b->behavior_dev == NULL) {
        strcpy(out, "—");
        return;
    }
    if (strcmp(b->behavior_dev, "none") == 0) {
        strcpy(out, "—");
        return;
    }
    if (strcmp(b->behavior_dev, "dynamic_macro") == 0) {
        const char *name = zmk_dynamic_macro_get_name(b->param1);

        if (name[0] != '\0') {
            snprintk(out, n, "%s", name);
        } else {
            snprintk(out, n, "M%lu", b->param1 + 1);
        }
        return;
    }
    if (strcmp(b->behavior_dev, "key_press") == 0) {
        key_press_label(out, n, b->param1, b->param2);
        return;
    }
    /* Composite bindings show both halves: "S/A", "MO1/ENT", "STK/G". */
    if (strcmp(b->behavior_dev, "mod_tap") == 0) {
        char base[8];
        char pre[5];

        keycode_base(base, sizeof(base), b->param2);
        mods_prefix(pre, sizeof(pre), b->param1 & 0x7F); /* bit7: right-hand mods */
        snprintk(out, n, "%s/%s", pre, base);
        return;
    }
    if (strcmp(b->behavior_dev, "layer_tap") == 0) {
        char base[8];

        keycode_base(base, sizeof(base), b->param2);
        snprintk(out, n, "MO%lu/%s", b->param1, base);
        return;
    }
    if (strcmp(b->behavior_dev, "sticky_key") == 0) {
        char base[8];

        keycode_base(base, sizeof(base), b->param1);
        snprintk(out, n, "STK/%s", base);
        return;
    }
    if (strcmp(b->behavior_dev, "bluetooth") == 0) {
        if (b->param1 <= 4) {
            snprintk(out, n, "BT%lu", b->param1); /* &bt 0..4 = profile slot */
        } else {
            strcpy(out, "BT"); /* SEL/NEXT/PREV/CLEAR... */
        }
        return;
    }
    if (strcmp(b->behavior_dev, "outputs") == 0) {
        strcpy(out, b->param1 == 0 ? "USB" : "BLE");
        return;
    }
    for (size_t i = 0; i < ARRAY_SIZE(DEV_LABELS); i++) {
        if (strcmp(b->behavior_dev, DEV_LABELS[i].dev) == 0) {
            snprintk(out, n, DEV_LABELS[i].fmt, b->param1);
            return;
        }
    }
    snprintk(out, n, "%.*s", (int)(n - 1), b->behavior_dev);
}

/* ------------------------------------------------------------------ */
/* Refresh (display work queue)                                         */
/* ------------------------------------------------------------------ */

static void dots_rebuild(unsigned int count);
static void pip_event_cb(lv_event_t *e);

static unsigned int layer_count(void) {
    unsigned int n = 0;

    while (n < ZMK_KEYMAP_LAYERS_LEN &&
           zmk_keymap_layer_index_to_id((zmk_keymap_layer_index_t)n) != ZMK_KEYMAP_LAYER_ID_INVAL) {
        n++;
    }
    return n;
}

static void dots_rebuild(unsigned int count) {
    for (unsigned int i = 0; i < ZMK_KEYMAP_LAYERS_LEN; i++) {
        if (dots[i] != NULL) {
            lv_obj_del(dots[i]); /* takes the index label with it */
            dots[i] = NULL;
        }
        dots_num[i] = NULL;
    }

    for (unsigned int i = 0; i < count; i++) {
        lv_obj_t *dot = lv_obj_create(dots_row);

        lv_obj_remove_style_all(dot);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE); /* see zmk_ui_flex */
        lv_obj_add_style(dot, zmk_display_style(ZMK_STYLE_LAYER_DOT), 0);
        /* Each pip is a key-sized button: tap pip n -> layer n (owner
         * pass 5; this LVGL makes lv_obj clickable by default, but be
         * explicit since we rely on it). */
        lv_obj_add_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        /* Invisible touch padding (owner pass 5: 48x32 is a small target):
         * +24 px on every side makes the hit box ~96x68 — adjacent pips'
         * zones butt up exactly (gap is 48), and vertically it spans from
         * the grid's bottom edge to the screen edge. Pixels unchanged. */
        lv_obj_set_ext_click_area(dot, TILE_GAP * 3);
        lv_obj_add_event_cb(dot, pip_event_cb, LV_EVENT_ALL, (void *)(uintptr_t)i);

        /* Layer number in the pip's centre (1-based for humans; ZMK still
         * calls these layers 0..n-1 in MO/TO bindings and Studio). */
        lv_obj_t *num = zmk_ui_label(dot, ZMK_STYLE_TEXT_LAYER_NUM);

        lv_obj_center(num);
        lv_label_set_text_fmt(num, "%u", i + 1);
        dots[i] = dot;
        dots_num[i] = num;
    }
    /* Fresh pips have no state styles: re-apply from scratch. */
    memset(dot_active_cache, 0, sizeof(dot_active_cache));
    dot_count = count;
}

static void refresh(void) {
    /* Boot race: settings load raises keymap/layer events long before the
     * screen manager has built this page (stats_screen.c pattern). */
    if (screen_root == NULL) {
        return;
    }

    zmk_keymap_layer_index_t cur = zmk_keymap_highest_layer_active();
    zmk_keymap_layer_id_t layer = zmk_keymap_layer_index_to_id(cur);
    unsigned int count = layer_count();
    char buf[16];

    if (count != dot_count) {
        dots_rebuild(count);
    }

    for (unsigned int i = 0; i < count; i++) {
        bool active = (i == (unsigned int)cur);

        if (dots[i] == NULL || dot_active_cache[i] == active) {
            continue;
        }
        dot_active_cache[i] = active;
        zmk_ui_style_toggle(dots[i], zmk_display_style(ZMK_STYLE_LAYER_DOT_ACTIVE), active);
        if (dots_num[i] != NULL) {
            zmk_ui_style_toggle(dots_num[i], zmk_display_style(ZMK_STYLE_TEXT_LAYER_NUM_ON),
                                active);
        }
    }

    if (layer == ZMK_KEYMAP_LAYER_ID_INVAL) {
        return;
    }

    for (unsigned int pos = 0; pos < ZMK_VKEY_CELLS; pos++) {
        if (tile_labels[pos] == NULL) {
            continue;
        }
        binding_label(pos + ZMK_PHYS_ROW, layer, buf, sizeof(buf));
        zmk_ui_set_text(tile_cache[pos], sizeof(tile_cache[pos]), tile_labels[pos], buf);
    }
}

static void lit_apply(void) {
    for (unsigned int pos = 0; pos < ZMK_VKEY_CELLS; pos++) {
        if (tiles[pos] == NULL) {
            continue;
        }
        zmk_ui_style_toggle(tiles[pos], zmk_display_style(ZMK_STYLE_KEY_TILE_PRESSED),
                            tile_lit[pos]);
    }
}

ZMK_UI_WORK_DEFINE(refresh_work, refresh_work_cb) {
    ARG_UNUSED(work);
    refresh();
}

ZMK_UI_WORK_DEFINE(lit_work, lit_work_cb) {
    ARG_UNUSED(work);
    lit_apply();
}

/* ------------------------------------------------------------------ */
/* Event plumbing                                                       */
/* ------------------------------------------------------------------ */

/* The grid mirrors the REAL layer (pad10a layer_state_set_user): every
 * layer change - hardware keys (h04), Studio, nav buttons - relabels. */
static int layer_changed_event_cb(const zmk_event_t *eh) {
    ARG_UNUSED(eh);
    zmk_ui_post(&refresh_work);
    return 0;
}

ZMK_LISTENER(keymap_screen, layer_changed_event_cb);
ZMK_SUBSCRIPTION(keymap_screen, zmk_layer_state_changed);

/* Bindings/layers changed under us (Studio RPC, vkey shell, settings). */
static int keymap_changed_event_cb(const zmk_event_t *eh) {
    ARG_UNUSED(eh);
    zmk_ui_post(&refresh_work);
    return 0;
}

ZMK_LISTENER(keymap_screen_tbl, keymap_changed_event_cb);
ZMK_SUBSCRIPTION(keymap_screen_tbl, zmk_keymap_changed);

/* Tile flash from the key events themselves (pad10a layers_cb): press-
 * mode macros hold the tile lit, hardware keys light it too. */
static int position_changed_event_cb(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);

    if (ev != NULL && ev->position >= ZMK_PHYS_ROW &&
        ev->position < ZMK_PHYS_ROW + ZMK_VKEY_CELLS) {
        tile_lit[ev->position - ZMK_PHYS_ROW] = ev->state;
        zmk_ui_post(&lit_work);
    }
    return 0;
}

ZMK_LISTENER(keymap_screen_pos, position_changed_event_cb);
ZMK_SUBSCRIPTION(keymap_screen_pos, zmk_position_state_changed);

/* Macro names show on the tiles: rename -> relabel (Studio or shell). */
static void macro_changed_cb(uint32_t macro_idx) {
    ARG_UNUSED(macro_idx);
    zmk_ui_post(&refresh_work);
}

/* ------------------------------------------------------------------ */
/* Tile input                                                           */
/* ------------------------------------------------------------------ */

static void tile_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    unsigned int pos = (unsigned int)(uintptr_t)lv_event_get_user_data(e);

    /* All 12 cells are plain keys: touch edges fire the position, the
     * flash follows the real key event in position_changed_event_cb. */
    switch (code) {
    case LV_EVENT_PRESSED:
        post_edge(pos, true);
        break;
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
        post_edge(pos, false);
        break;
    default:
        break;
    }
}

/* Each layer pip behaves like a normal key (owner pass 5): its own fill
 * lights up under the finger (key-tile press style) and falls back on
 * release/press-lost; releasing on the pip jumps straight to that layer. */
static void pip_event_cb(lv_event_t *e) {
    lv_obj_t *pip = lv_event_get_target(e);
    unsigned int idx = (unsigned int)(uintptr_t)lv_event_get_user_data(e);

    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        lv_obj_add_style(pip, zmk_display_style(ZMK_STYLE_LAYER_DOT_PRESSED), 0);
        break;
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
        lv_obj_remove_style(pip, zmk_display_style(ZMK_STYLE_LAYER_DOT_PRESSED), 0);
        break;
    case LV_EVENT_CLICKED:
        nav_goto = idx;
        nav_cmd = NAV_GOTO;
        zmk_ui_post(&nav_work);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Build                                                                */
/* ------------------------------------------------------------------ */

static lv_obj_t *keymap_page_create(lv_obj_t *parent) {
    screen_root = lv_obj_create(parent);
    lv_obj_remove_style_all(screen_root);
    lv_obj_remove_flag(screen_root, LV_OBJ_FLAG_SCROLLABLE); /* see zmk_ui_flex */
    lv_obj_set_size(screen_root, lv_pct(100), lv_pct(100));

#if IS_ENABLED(CONFIG_ZMK_DISPLAY_ANIM)
    /* a12 owner pass: animated background under the tiles (the bench
     * checks touch still feels clean with the decoder running). */
    zmk_anim_bg_attach(screen_root, PAGE_NAME);
#endif

    /* Layer indicator (bottom band, owner pass 5): the band itself is a
     * passive frame — the individual pips are the buttons (tap pip n ->
     * layer n, key-style press fill); ‹/› step prev/next with wrap. */
    lv_obj_t *band = lv_obj_create(screen_root);

    lv_obj_remove_style_all(band);
    lv_obj_set_size(band, ZMK_VKEY_COLS * TILE_W + (ZMK_VKEY_COLS - 1) * TILE_GAP, BAND_H);
    lv_obj_align(band, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_remove_flag(band, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(band, LV_OBJ_FLAG_SCROLLABLE); /* see zmk_ui_flex */

    dots_row = zmk_ui_flex(band, LV_FLEX_FLOW_ROW, TILE_GAP * 6);
    lv_obj_remove_flag(dots_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(dots_row);

    for (unsigned int pos = 0; pos < ZMK_VKEY_CELLS; pos++) {
        unsigned int row = pos / ZMK_VKEY_COLS;
        unsigned int col = pos % ZMK_VKEY_COLS;

        lv_obj_t *tile = lv_obj_create(screen_root);

        lv_obj_remove_style_all(tile);
        lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE); /* see zmk_ui_flex */
        lv_obj_add_style(tile, zmk_display_style(ZMK_STYLE_KEY_TILE), 0);
        lv_obj_set_size(tile, TILE_W, TILE_H);
        lv_obj_set_pos(tile, GRID_X + col * (TILE_W + TILE_GAP),
                       GRID_Y + row * (TILE_H + TILE_GAP));
        lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
        /* user_data = the REAL keymap position (physical row offsets it) */
        lv_obj_add_event_cb(tile, tile_event_cb, LV_EVENT_ALL,
                            (void *)(uintptr_t)(pos + ZMK_PHYS_ROW));
        tiles[pos] = tile;
        tile_lit[pos] = false;

        lv_obj_t *lbl = zmk_ui_label(tile, ZMK_STYLE_TEXT_KEY_LABEL);

        lv_obj_set_width(lbl, TILE_W - TILE_GAP);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
        lv_obj_center(lbl);
        tile_labels[pos] = lbl;
    }

    zmk_dynamic_macro_add_listener(macro_changed_cb);
    dots_rebuild(layer_count());
    refresh();
    return parent;
}

ZMK_DISPLAY_DEFINE_SCREEN(keymap, PAGE_NAME, keymap_page_create, 40);
