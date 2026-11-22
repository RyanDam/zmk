/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Coban Tab 1a display design system — 5-slot theme model (backlog a07).
 *
 * Port of pad10a's `theme_color_t` (background, text_primary, text_secondary,
 * accent, inactive) scaled for the 480×320 canvas. The theme is the ONLY source of
 * colors for screen code: styles.c derives every lv_style_t from it, and the
 * model persists via ZMK settings ("display/theme/colors", 15 bytes RGB888,
 * same wire shape as pad10a's eeprom blob) so the Studio RPC (a14) can edit
 * it from the host later.
 *
 * NOTE: this file replaces the old vestigial theme.[ch], which only selected
 * a small font for the monochrome LVGL theme and was not even compiled. The
 * font-selection macros survive at the bottom for mono displays.
 */

#pragma once

#include <lvgl.h>

struct zmk_display_theme {
    lv_color_t background;
    lv_color_t text_primary;
    lv_color_t text_secondary;
    lv_color_t accent;
    lv_color_t inactive;
};

/* The live theme. Read-only for screens — go through zmk_display_style()
 * instead; this accessor exists for style rebuilding and diagnostics. */
const struct zmk_display_theme *zmk_display_theme_get(void);

/* Boot-time init: seed from Kconfig defaults, build the style registry.
 * Called by the display module before the status screen is created. */
int zmk_display_theme_init(void);

/* Replace the theme (live re-render of every styled object) and persist it
 * via settings. Safe from any thread: LVGL work is posted to the display
 * work queue. Settings errors are logged but not returned — the live apply
 * still succeeds (persistence is gated by c02 on tab1a). */
int zmk_display_theme_set(const struct zmk_display_theme *theme);

/* Cycle through built-in preset palettes (bench seam test until d05/h04
 * give better live-edit input). */
void zmk_display_theme_cycle_preset(void);

/*
 * Legacy small-font selector (monochrome theme only).
 */

#if defined(CONFIG_ZMK_LV_FONT_DEFAULT_SMALL_MONTSERRAT_8)
#define CONFIG_ZMK_LV_FONT_DEFAULT_SMALL &lv_font_montserrat_8
#elif defined(CONFIG_ZMK_LV_FONT_DEFAULT_SMALL_MONTSERRAT_10)
#define CONFIG_ZMK_LV_FONT_DEFAULT_SMALL &lv_font_montserrat_10
#elif defined(CONFIG_ZMK_LV_FONT_DEFAULT_SMALL_MONTSERRAT_12)
#define CONFIG_ZMK_LV_FONT_DEFAULT_SMALL &lv_font_montserrat_12
#elif defined(CONFIG_ZMK_LV_FONT_DEFAULT_SMALL_MONTSERRAT_14)
#define CONFIG_ZMK_LV_FONT_DEFAULT_SMALL &lv_font_montserrat_14
#elif defined(CONFIG_ZMK_LV_FONT_DEFAULT_SMALL_MONTSERRAT_16)
#define CONFIG_ZMK_LV_FONT_DEFAULT_SMALL &lv_font_montserrat_16
#elif defined(CONFIG_ZMK_LV_FONT_DEFAULT_SMALL_MONTSERRAT_18)
#define CONFIG_ZMK_LV_FONT_DEFAULT_SMALL &lv_font_montserrat_18
#elif defined(CONFIG_ZMK_LV_FONT_DEFAULT_SMALL_MONTSERRAT_20)
#define CONFIG_ZMK_LV_FONT_DEFAULT_SMALL &lv_font_montserrat_20
#elif defined(CONFIG_ZMK_LV_FONT_DEFAULT_SMALL_MONTSERRAT_22)
#define CONFIG_ZMK_LV_FONT_DEFAULT_SMALL &lv_font_montserrat_22
#elif defined(CONFIG_ZMK_LV_FONT_DEFAULT_SMALL_MONTSERRAT_24)
#define CONFIG_ZMK_LV_FONT_DEFAULT_SMALL &lv_font_montserrat_24
#elif defined(CONFIG_ZMK_LV_FONT_DEFAULT_SMALL_MONTSERRAT_26)
#define CONFIG_ZMK_LV_FONT_DEFAULT_SMALL &lv_font_montserrat_26
#elif defined(CONFIG_ZMK_LV_FONT_DEFAULT_SMALL_MONTSERRAT_28)
#define CONFIG_ZMK_LV_FONT_DEFAULT_SMALL &lv_font_montserrat_28
#elif defined(CONFIG_ZMK_LV_FONT_DEFAULT_SMALL_MONTSERRAT_30)
#define CONFIG_ZMK_LV_FONT_DEFAULT_SMALL &lv_font_montserrat_30
#elif defined(CONFIG_ZMK_LV_FONT_DEFAULT_SMALL_MONTSERRAT_32)
#define CONFIG_ZMK_LV_FONT_DEFAULT_SMALL &lv_font_montserrat_32
#elif defined(CONFIG_ZMK_LV_FONT_DEFAULT_SMALL_MONTSERRAT_34)
#define CONFIG_ZMK_LV_FONT_DEFAULT_SMALL &lv_font_montserrat_34
#elif defined(CONFIG_ZMK_LV_FONT_DEFAULT_SMALL_MONTSERRAT_36)
#define CONFIG_ZMK_LV_FONT_DEFAULT_SMALL &lv_font_montserrat_36
#elif defined(CONFIG_ZMK_LV_FONT_DEFAULT_SMALL_MONTSERRAT_38)
#define CONFIG_ZMK_LV_FONT_DEFAULT_SMALL &lv_font_montserrat_38
#elif defined(CONFIG_ZMK_LV_FONT_DEFAULT_SMALL_MONTSERRAT_40)
#define CONFIG_ZMK_LV_FONT_DEFAULT_SMALL &lv_font_montserrat_40
#elif defined(CONFIG_ZMK_LV_FONT_DEFAULT_SMALL_MONTSERRAT_42)
#define CONFIG_ZMK_LV_FONT_DEFAULT_SMALL &lv_font_montserrat_42
#elif defined(CONFIG_ZMK_LV_FONT_DEFAULT_SMALL_MONTSERRAT_44)
#define CONFIG_ZMK_LV_FONT_DEFAULT_SMALL &lv_font_montserrat_44
#elif defined(CONFIG_ZMK_LV_FONT_DEFAULT_SMALL_MONTSERRAT_46)
#define CONFIG_ZMK_LV_FONT_DEFAULT_SMALL &lv_font_montserrat_46
#elif defined(CONFIG_ZMK_LV_FONT_DEFAULT_SMALL_MONTSERRAT_48)
#define CONFIG_ZMK_LV_FONT_DEFAULT_SMALL &lv_font_montserrat_48
#endif
