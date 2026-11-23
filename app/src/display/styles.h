/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Coban Tab 1a design system — shared style registry + 480×320 landscape
 * grid tokens (backlog a07).
 *
 * THE RULE: screens get colors, fonts and sizes ONLY from this registry.
 * A hardcoded lv_color_* / font symbol in screen code is a review reject
 * (same discipline pad10a enforced with graphics/screens/styles.h).
 *
 * Generated fonts live in fonts/ (see ../../../scripts/display-fonts/
 * gen_fonts.sh); the size ramp + grid are documented in grid.md.
 */

#pragma once

#include <lvgl.h>

#include "theme.h"

/* ------------------------------------------------------------------ */
/* 480×320 landscape grid (see grid.md)                                */
/* ------------------------------------------------------------------ */

#define ZMK_GRID_UNIT 8                       /* base grid unit, px            */
#define ZMK_GRID_MARGIN 0                     /* safe margin all sides   */
#define ZMK_GRID_TOUCH_MIN 44                 /* min touch target, px          */
#define ZMK_GRID_REACH_H (ZMK_GRID_UNIT * 20) /* bottom-weighted reach zone */
/* Logical landscape canvas: the 320x480 panel presented at 90° — the node's
 * `rotation` property is what makes W/H mean what they say (grid.md). */
#define ZMK_GRID_SCREEN_W 480
#define ZMK_GRID_SCREEN_H 320

/* ------------------------------------------------------------------ */
/* Typography ramp (sizes baked into the generated fonts)              */
/* ------------------------------------------------------------------ */

extern const lv_font_t lv_font_tab1a_baijamjuree_144;
extern const lv_font_t lv_font_tab1a_baijamjuree_72;
extern const lv_font_t lv_font_tab1a_baijamjuree_48;
extern const lv_font_t lv_font_tab1a_dmserif_48;
extern const lv_font_t lv_font_tab1a_inter_28;
extern const lv_font_t lv_font_tab1a_inter_20;
extern const lv_font_t lv_font_tab1a_inter_16;
extern const lv_font_t lv_font_tab1a_barlow_24;
extern const lv_font_t lv_font_tab1a_barlow_20;

/* ------------------------------------------------------------------ */
/* Style registry                                                      */
/* ------------------------------------------------------------------ */

enum zmk_display_style_id {
    ZMK_STYLE_SCREEN,         /* page background                 */
    ZMK_STYLE_TEXT_HERO_XL,   /* 144 baijamjuree (hero clock, wave 1) */
    ZMK_STYLE_TEXT_HERO,      /* 72 baijamjuree  (fusion stacked clock) */
    ZMK_STYLE_TEXT_HERO_ALT,  /* 48 dmserif      (clock style 2) */
    ZMK_STYLE_TEXT_SUBHERO,   /* 48 baijamjuree  (side clock, timers) */
    ZMK_STYLE_TEXT_TITLE,     /* 28 inter        (screen titles) */
    ZMK_STYLE_TEXT_TITLE_SEC, /* 28 inter, secondary color (calendar year) */
    ZMK_STYLE_TEXT_BODY,      /* 20 inter        (body/date)     */
    ZMK_STYLE_TEXT_CAPTION,   /* 16 inter        (hints/labels)  */
    ZMK_STYLE_TEXT_TABULAR,   /* 20 barlow       (stats numbers) */
    ZMK_STYLE_TEXT_DAY,       /* 24 barlow       (calendar day cells) */
    ZMK_STYLE_TEXT_DAY_DIM,   /* 24 barlow, 50 % text (adjacent months) */
    ZMK_STYLE_TEXT_DAY_BOLD,  /* 24 barlow, 1 px fake bold (no bold face
                               * in the generated ramp): header year/month
                               * and today's digit */
    ZMK_STYLE_TEXT_SECONDARY, /* body color override: secondary  */
    ZMK_STYLE_TEXT_ON_ACCENT, /* primary text on accent fill     */
    ZMK_STYLE_ACCENT,         /* accent fill (bars, indicators)  */
    ZMK_STYLE_INACTIVE,       /* inactive fill (bar bg, idle)    */
    ZMK_STYLE_TODAY,          /* calendar today pip: white 25 % circle */
    ZMK_STYLE_POMO_BREATH,    /* white 10 % disc (idle/rest breathe, a10) */
    ZMK_STYLE_POMO_BREATH_HOLD, /* same disc at 30 % while held (a10)  */
    ZMK_STYLE_BAR_MAIN,       /* session bar: block pill track (a10) */
    ZMK_STYLE_BAR_IND,        /* session bar: work block fill  (a10) */
    ZMK_STYLE_BAR_REST_IND,   /* session bar: rest block fill  (a10) */
    ZMK_STYLE_RING,           /* arc track (bg) part             */
    ZMK_STYLE_RING_INDICATOR, /* arc progress (indicator part)   */
    ZMK_STYLE_TOUCH,          /* interactive target base state   */
    ZMK_STYLE_TOUCH_PRESSED,  /* interactive target pressed      */
    ZMK_STYLE_TOUCH_GHOST,    /* invisible 44 px hit-target (a10 Cancel) */
    ZMK_STYLE_SCRIM,          /* modal dim: black 70 % (a10 dialog)      */
    ZMK_STYLE_CARD,           /* modal card: screen bg, rounded, padded  */
    ZMK_STYLE_COUNT,
};

/* Pointer into the registry; valid after zmk_display_theme_init(). */
const lv_style_t *zmk_display_style(enum zmk_display_style_id id);

/* Rebuild every style from the given theme. Call on boot and whenever the
 * theme changes; callers that are not on the display work queue must use
 * zmk_display_theme_set()/zmk_display_theme_apply() instead. */
void zmk_display_styles_apply_theme(const struct zmk_display_theme *theme);
