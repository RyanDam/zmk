/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * 5-slot theme model implementation (backlog a07).
 *
 * Wire format matches pad10a's persisted theme (eeprom/cb_eeprom.h:55-61):
 * 5 slots × RGB888 = 15 raw bytes, so a14 host tooling can port the format.
 *
 * Persistence caveat (rule R6 / backlog c02): on tab1a today the storage
 * partition fails, so settings_save_one() logs a warning and the theme still
 * applies live. Compile-time Kconfig defaults are the source of truth until
 * c02 lands.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>

#include <lvgl.h>

#include <zmk/display.h>

#include "theme.h"
#include "styles.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/settings_blob.h>

#if IS_ENABLED(CONFIG_SETTINGS)
#define THEME_SETTINGS_SUBTREE "display/theme"
#define THEME_SETTINGS_SUBKEY "colors"
#define THEME_SETTINGS_KEY THEME_SETTINGS_SUBTREE "/" THEME_SETTINGS_SUBKEY

struct theme_settings_blob {
    uint8_t rgb[5][3]; /* background, text_primary, text_secondary, accent, inactive */
};
#endif

static struct zmk_display_theme current_theme;

static void apply_fn(struct k_work *work);

/* Statically defined on purpose: the settings-load path can submit this
 * before display init runs, and a bare `struct k_work` would still have a
 * NULL handler then — sysworkq jumps to 0 and the whole chip freezes. */
K_WORK_DEFINE(theme_apply_work, apply_fn);

static void put_color(uint8_t *dst, lv_color_t c) {
    dst[0] = c.red;
    dst[1] = c.green;
    dst[2] = c.blue;
}

static lv_color_t get_color(const uint8_t *src) { return lv_color_make(src[0], src[1], src[2]); }

static void apply_fn(struct k_work *work) {
    zmk_display_styles_apply_theme(&current_theme);
    /* Shared styles were mutated in place — invalidate every object that
     * references any style so the full-refresh panel repaints. */
    lv_obj_report_style_change(NULL);
}

static void theme_schedule_apply(void) {
    k_work_submit_to_queue(zmk_display_work_q(), &theme_apply_work);
}

const struct zmk_display_theme *zmk_display_theme_get(void) { return &current_theme; }

static void theme_from_defaults(void) {
    current_theme.background = lv_color_hex(CONFIG_ZMK_DISPLAY_THEME_BACKGROUND);
    current_theme.text_primary = lv_color_hex(CONFIG_ZMK_DISPLAY_THEME_TEXT_PRIMARY);
    current_theme.text_secondary = lv_color_hex(CONFIG_ZMK_DISPLAY_THEME_TEXT_SECONDARY);
    current_theme.accent = lv_color_hex(CONFIG_ZMK_DISPLAY_THEME_ACCENT);
    current_theme.inactive = lv_color_hex(CONFIG_ZMK_DISPLAY_THEME_INACTIVE);
}

#if IS_ENABLED(CONFIG_SETTINGS)
static int theme_settings_handle_set(const char *key, size_t len, settings_read_cb read_cb,
                                     void *cb_arg) {
    struct theme_settings_blob blob;
    int err = zmk_settings_blob_read(key, len, read_cb, cb_arg, THEME_SETTINGS_SUBKEY, &blob,
                                     sizeof(blob), "display theme");

    if (err <= 0) {
        return err;
    }

    current_theme.background = get_color(blob.rgb[0]);
    current_theme.text_primary = get_color(blob.rgb[1]);
    current_theme.text_secondary = get_color(blob.rgb[2]);
    current_theme.accent = get_color(blob.rgb[3]);
    current_theme.inactive = get_color(blob.rgb[4]);

    LOG_INF("display theme loaded from settings");
    theme_schedule_apply();
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(display_theme, THEME_SETTINGS_SUBTREE, NULL,
                               theme_settings_handle_set, NULL, NULL);

static void theme_settings_save(void) {
    struct theme_settings_blob blob;

    put_color(blob.rgb[0], current_theme.background);
    put_color(blob.rgb[1], current_theme.text_primary);
    put_color(blob.rgb[2], current_theme.text_secondary);
    put_color(blob.rgb[3], current_theme.accent);
    put_color(blob.rgb[4], current_theme.inactive);

    zmk_settings_blob_save(THEME_SETTINGS_KEY, &blob, sizeof(blob), "display theme");
}
#else
static void theme_settings_save(void) {}
#endif /* CONFIG_SETTINGS */

int zmk_display_theme_set(const struct zmk_display_theme *theme) {
    if (theme == NULL) {
        return -EINVAL;
    }

    current_theme = *theme;
    theme_settings_save();
    theme_schedule_apply();
    return 0;
}

/* Built-in presets (RGB888 ints — lv_color_hex() is a function); [0] must
 * equal the Kconfig defaults (0x14161C / …). */
struct theme_preset {
    uint32_t background, text_primary, text_secondary, accent, inactive;
};

static const struct theme_preset presets[] = {
    {0x14161C, 0xE6E6E6, 0x8A8F98, 0xE8A33D, 0x3A3F49}, /* "Night Amber" — default */
    {0xF2F0E8, 0x22242A, 0x6E7278, 0xC25B3A, 0xD8D4C6}, /* "Paper" */
    {0x0B1F2A, 0xDCE9EE, 0x7A95A0, 0x3FBFA4, 0x24404D}, /* "Teal Desk" */
};

static size_t preset_index;

void zmk_display_theme_cycle_preset(void) {
    const struct theme_preset *p;

    preset_index = (preset_index + 1) % ARRAY_SIZE(presets);
    p = &presets[preset_index];

    current_theme.background = lv_color_hex(p->background);
    current_theme.text_primary = lv_color_hex(p->text_primary);
    current_theme.text_secondary = lv_color_hex(p->text_secondary);
    current_theme.accent = lv_color_hex(p->accent);
    current_theme.inactive = lv_color_hex(p->inactive);

    theme_settings_save();
    theme_schedule_apply();
    LOG_INF("display theme preset %u", (unsigned int)preset_index);
}

int zmk_display_theme_init(void) {
    theme_from_defaults();
    zmk_display_styles_apply_theme(&current_theme);
    return 0;
}
