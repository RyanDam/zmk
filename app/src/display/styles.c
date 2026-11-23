/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Style registry implementation (backlog a07) — every lv_style_t is a pure
 * function of the 5-slot theme + the fixed typography ramp, mirroring
 * pad10a's init_styles()/apply_theme() pattern (pad10a
 * graphics/screens/styles.c:199-217) for the 480×320 landscape canvas.
 */

#include <string.h>

#include <zephyr/sys/util.h>

#include <lvgl.h>

#include "styles.h"

static lv_style_t styles[ZMK_STYLE_COUNT];

void zmk_display_styles_apply_theme(const struct zmk_display_theme *t) {
    const lv_color_t on_accent = lv_color_white();

    /* screen */
    lv_style_reset(&styles[ZMK_STYLE_SCREEN]);
    lv_style_set_bg_color(&styles[ZMK_STYLE_SCREEN], t->background);
    lv_style_set_bg_opa(&styles[ZMK_STYLE_SCREEN], LV_OPA_COVER);
    lv_style_set_border_width(&styles[ZMK_STYLE_SCREEN], 0);
    lv_style_set_pad_all(&styles[ZMK_STYLE_SCREEN], ZMK_GRID_MARGIN);

    /* text ramp: size+face fixed, color from theme */
    struct {
        enum zmk_display_style_id id;
        const lv_font_t *font;
        lv_color_t color;
    } texts[] = {
        {ZMK_STYLE_TEXT_HERO_XL, &lv_font_tab1a_baijamjuree_144, t->text_primary},
        {ZMK_STYLE_TEXT_HERO, &lv_font_tab1a_baijamjuree_72, t->text_primary},
        {ZMK_STYLE_TEXT_HERO_ALT, &lv_font_tab1a_dmserif_48, t->text_primary},
        {ZMK_STYLE_TEXT_SUBHERO, &lv_font_tab1a_baijamjuree_48, t->text_primary},
        {ZMK_STYLE_TEXT_TITLE, &lv_font_tab1a_inter_28, t->text_primary},
        {ZMK_STYLE_TEXT_TITLE_SEC, &lv_font_tab1a_inter_28, t->text_secondary},
        {ZMK_STYLE_TEXT_BODY, &lv_font_tab1a_inter_20, t->text_primary},
        {ZMK_STYLE_TEXT_CAPTION, &lv_font_tab1a_inter_16, t->text_secondary},
        {ZMK_STYLE_TEXT_TABULAR, &lv_font_tab1a_barlow_20, t->text_primary},
        {ZMK_STYLE_TEXT_DAY, &lv_font_tab1a_barlow_24, t->text_primary},
        {ZMK_STYLE_TEXT_DAY_DIM, &lv_font_tab1a_barlow_24, t->text_primary},
        {ZMK_STYLE_TEXT_DAY_BOLD, &lv_font_tab1a_barlow_24, t->text_primary},
        {ZMK_STYLE_TEXT_SECONDARY, &lv_font_tab1a_inter_20, t->text_secondary},
        {ZMK_STYLE_TEXT_ON_ACCENT, &lv_font_tab1a_inter_20, on_accent},
    };

    for (size_t i = 0; i < ARRAY_SIZE(texts); i++) {
        lv_style_reset(&styles[texts[i].id]);
        lv_style_set_text_font(&styles[texts[i].id], texts[i].font);
        lv_style_set_text_color(&styles[texts[i].id], texts[i].color);
    }

    /* Adjacent-month days read as one dim style instead of a per-object
     * opacity poke (the a07 rule: styling lives in the registry). */
    lv_style_set_text_opa(&styles[ZMK_STYLE_TEXT_DAY_DIM], LV_OPA_50);

    /* Fake bold (header year/month + today): no bold face ships in the
     * generated ramp, so a 1 px outline stroke in the text's own colour
     * thickens the strokes. Purely paint — glyph boxes and layout do
     * not move. */
    lv_style_set_text_outline_stroke_width(&styles[ZMK_STYLE_TEXT_DAY_BOLD], 1);
    lv_style_set_text_outline_stroke_color(&styles[ZMK_STYLE_TEXT_DAY_BOLD], t->text_primary);

    /* fills */
    lv_style_reset(&styles[ZMK_STYLE_ACCENT]);
    lv_style_set_bg_color(&styles[ZMK_STYLE_ACCENT], t->accent);
    lv_style_set_bg_opa(&styles[ZMK_STYLE_ACCENT], LV_OPA_COVER);
    lv_style_set_border_width(&styles[ZMK_STYLE_ACCENT], 0);

    lv_style_reset(&styles[ZMK_STYLE_INACTIVE]);
    lv_style_set_bg_color(&styles[ZMK_STYLE_INACTIVE], t->inactive);
    lv_style_set_bg_opa(&styles[ZMK_STYLE_INACTIVE], LV_OPA_COVER);
    lv_style_set_border_width(&styles[ZMK_STYLE_INACTIVE], 0);

    /* calendar today pip: a whisper-white disc behind the date (owner
     * wave-1 pass) — theme-independent, so no t-> slot */
    lv_style_reset(&styles[ZMK_STYLE_TODAY]);
    lv_style_set_bg_color(&styles[ZMK_STYLE_TODAY], lv_color_white());
    lv_style_set_bg_opa(&styles[ZMK_STYLE_TODAY], (lv_opa_t)(255 * 25 / 100)); /* exactly 25 % */
    lv_style_set_radius(&styles[ZMK_STYLE_TODAY], LV_RADIUS_CIRCLE);
    lv_style_set_border_width(&styles[ZMK_STYLE_TODAY], 0);

    /* Pomodoro breath disc: whisper-white at 10 % (theme-independent,
     * no tint — owner pass 2026-11-23; pad10a's was the accent hue,
     * style_pomo_breath:185-187). The HOLD variant's 30 % is the
     * hold-to-start feedback. Radius circle lives here so growth never
     * needs a per-tick radius poke. */
    lv_style_reset(&styles[ZMK_STYLE_POMO_BREATH]);
    lv_style_set_bg_color(&styles[ZMK_STYLE_POMO_BREATH], lv_color_white());
    lv_style_set_bg_opa(&styles[ZMK_STYLE_POMO_BREATH], LV_OPA_10);
    lv_style_set_radius(&styles[ZMK_STYLE_POMO_BREATH], LV_RADIUS_CIRCLE);
    lv_style_set_border_width(&styles[ZMK_STYLE_POMO_BREATH], 0);

    lv_style_reset(&styles[ZMK_STYLE_POMO_BREATH_HOLD]);
    lv_style_set_bg_color(&styles[ZMK_STYLE_POMO_BREATH_HOLD], lv_color_white());
    lv_style_set_bg_opa(&styles[ZMK_STYLE_POMO_BREATH_HOLD], LV_OPA_30);
    lv_style_set_radius(&styles[ZMK_STYLE_POMO_BREATH_HOLD], LV_RADIUS_CIRCLE);
    lv_style_set_border_width(&styles[ZMK_STYLE_POMO_BREATH_HOLD], 0);

    /* Segmented session bar (a10). Each block is a whisper-white 10 %
     * pill (theme-independent, same family as the today pip) so the
     * session's shape reads as quiet furniture; fills tell the phases:
     * work accent, rest accent 40 % (pad10a's bar was the plain accent
     * pair). Pill = radius circle on both track and fill. */
    lv_style_reset(&styles[ZMK_STYLE_BAR_MAIN]);
    lv_style_set_bg_color(&styles[ZMK_STYLE_BAR_MAIN], lv_color_white());
    lv_style_set_bg_opa(&styles[ZMK_STYLE_BAR_MAIN], LV_OPA_10);
    lv_style_set_border_width(&styles[ZMK_STYLE_BAR_MAIN], 0);
    lv_style_set_radius(&styles[ZMK_STYLE_BAR_MAIN], LV_RADIUS_CIRCLE);

    lv_style_reset(&styles[ZMK_STYLE_BAR_IND]);
    lv_style_set_bg_color(&styles[ZMK_STYLE_BAR_IND], t->accent);
    lv_style_set_bg_opa(&styles[ZMK_STYLE_BAR_IND], LV_OPA_COVER);
    lv_style_set_border_width(&styles[ZMK_STYLE_BAR_IND], 0);
    lv_style_set_radius(&styles[ZMK_STYLE_BAR_IND], LV_RADIUS_CIRCLE);

    lv_style_reset(&styles[ZMK_STYLE_BAR_REST_IND]);
    lv_style_set_bg_color(&styles[ZMK_STYLE_BAR_REST_IND], t->accent);
    lv_style_set_bg_opa(&styles[ZMK_STYLE_BAR_REST_IND], LV_OPA_40);
    lv_style_set_border_width(&styles[ZMK_STYLE_BAR_REST_IND], 0);
    lv_style_set_radius(&styles[ZMK_STYLE_BAR_REST_IND], LV_RADIUS_CIRCLE);

    /* progress rings (a10): LVGL 9 draws the track on LV_PART_MAIN and
     * the progress on LV_PART_INDICATOR, both via arc_color/arc_width */
    lv_style_reset(&styles[ZMK_STYLE_RING]);
    lv_style_set_arc_color(&styles[ZMK_STYLE_RING], t->inactive);
    lv_style_set_arc_width(&styles[ZMK_STYLE_RING], ZMK_GRID_UNIT + ZMK_GRID_UNIT / 2);

    lv_style_reset(&styles[ZMK_STYLE_RING_INDICATOR]);
    lv_style_set_arc_color(&styles[ZMK_STYLE_RING_INDICATOR], t->accent);
    lv_style_set_arc_width(&styles[ZMK_STYLE_RING_INDICATOR], ZMK_GRID_UNIT + ZMK_GRID_UNIT / 2);

    /* touch targets */
    lv_style_reset(&styles[ZMK_STYLE_TOUCH]);
    lv_style_set_bg_color(&styles[ZMK_STYLE_TOUCH], t->inactive);
    lv_style_set_bg_opa(&styles[ZMK_STYLE_TOUCH], LV_OPA_COVER);
    lv_style_set_radius(&styles[ZMK_STYLE_TOUCH], ZMK_GRID_UNIT);
    lv_style_set_min_width(&styles[ZMK_STYLE_TOUCH], ZMK_GRID_TOUCH_MIN);
    lv_style_set_min_height(&styles[ZMK_STYLE_TOUCH], ZMK_GRID_TOUCH_MIN);
    lv_style_set_border_width(&styles[ZMK_STYLE_TOUCH], 0);

    lv_style_reset(&styles[ZMK_STYLE_TOUCH_PRESSED]);
    lv_style_set_bg_color(&styles[ZMK_STYLE_TOUCH_PRESSED], t->accent);
    lv_style_set_bg_opa(&styles[ZMK_STYLE_TOUCH_PRESSED], LV_OPA_COVER);
    lv_style_set_radius(&styles[ZMK_STYLE_TOUCH_PRESSED], ZMK_GRID_UNIT);
    lv_style_set_min_width(&styles[ZMK_STYLE_TOUCH_PRESSED], ZMK_GRID_TOUCH_MIN);
    lv_style_set_min_height(&styles[ZMK_STYLE_TOUCH_PRESSED], ZMK_GRID_TOUCH_MIN);
    lv_style_set_border_width(&styles[ZMK_STYLE_TOUCH_PRESSED], 0);

    /* Ghost hit-target (a10 Cancel, owner pass: the solid pill was too
     * loud on the session page): invisible 44 px box, the label inside
     * carries the affordance. */
    lv_style_reset(&styles[ZMK_STYLE_TOUCH_GHOST]);
    lv_style_set_bg_opa(&styles[ZMK_STYLE_TOUCH_GHOST], LV_OPA_TRANSP);
    lv_style_set_border_width(&styles[ZMK_STYLE_TOUCH_GHOST], 0);
    lv_style_set_radius(&styles[ZMK_STYLE_TOUCH_GHOST], ZMK_GRID_UNIT);
    lv_style_set_min_width(&styles[ZMK_STYLE_TOUCH_GHOST], ZMK_GRID_TOUCH_MIN);
    lv_style_set_min_height(&styles[ZMK_STYLE_TOUCH_GHOST], ZMK_GRID_TOUCH_MIN);

    /* Confirm dialog (a10, owner pass 2026-11-23 replacing hold-to-
     * start / ghost-cancel): scrim dims everything (and swallows the
     * taps), the card carries the question. */
    lv_style_reset(&styles[ZMK_STYLE_SCRIM]);
    lv_style_set_bg_color(&styles[ZMK_STYLE_SCRIM], lv_color_black());
    lv_style_set_bg_opa(&styles[ZMK_STYLE_SCRIM], LV_OPA_70);
    lv_style_set_border_width(&styles[ZMK_STYLE_SCRIM], 0);

    lv_style_reset(&styles[ZMK_STYLE_CARD]);
    lv_style_set_bg_color(&styles[ZMK_STYLE_CARD], t->background);
    lv_style_set_bg_opa(&styles[ZMK_STYLE_CARD], LV_OPA_COVER);
    lv_style_set_radius(&styles[ZMK_STYLE_CARD], ZMK_GRID_UNIT * 2);
    lv_style_set_pad_all(&styles[ZMK_STYLE_CARD], ZMK_GRID_UNIT * 2);
    lv_style_set_border_width(&styles[ZMK_STYLE_CARD], 0);
}

const lv_style_t *zmk_display_style(enum zmk_display_style_id id) {
    if (id >= ZMK_STYLE_COUNT) {
        return NULL;
    }
    return &styles[id];
}
