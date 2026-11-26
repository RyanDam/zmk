/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Asset-backed animation helpers, internal to src/display/: page backgrounds
 * and the dedicated animation page consume them; the store itself is the
 * public zmk/assets.h seam. All functions run on the display work queue.
 */

#pragma once

#include <stdbool.h>

#include <lvgl.h>

/** Create the named GIF centred on @p parent, sized as authored.
 *  @retval NULL asset missing, not a GIF, or allocation failed */
lv_obj_t *zmk_anim_gif_create(lv_obj_t *parent, const char *name);

/** True if a named asset exists and this build can play it. */
bool zmk_anim_available(const char *name);

/**
 * Scrim over the page-background GIF so text stays readable over busy art.
 * Raw LVGL opacity 0-255 (black), 0 disables, default
 * ZMK_DISPLAY_ANIM_TINT_OPA. zmk_anim_tint_set() applies within one 200 ms
 * tick and persists at settings display/anim/tint.
 */
uint8_t zmk_anim_tint_get(void);
void zmk_anim_tint_set(uint8_t opa);

/** Create the named GIF *or* Lottie (dispatch on the asset kind). */
lv_obj_t *zmk_anim_create(lv_obj_t *parent, const char *name);

/* Animated background setting (all pages), persisted at settings
 * "display/anim" key "bg"; RPC or `animbg on|off` toggles it. */
bool zmk_anim_bg_enabled(void);
void zmk_anim_bg_set(bool on);

/**
 * Which GIF the shared background shows and the anim page defaults to
 * (default "animbg"). zmk_anim_active_set() validates, persists at settings
 * "display/anim" key "active" and swaps within one tick.
 * @retval -EINVAL unknown/non-GIF asset, 0 ok.
 */
const char *zmk_anim_active_get(void);
int zmk_anim_active_set(const char *name);

/**
 * Register @p parent as bg page @p page_name for the shared background player.
 * One SHARED player is re-parented to the visible page: a player per page would
 * blow the PSRAM LVGL heap, since pages persist after their first visit. The
 * background is a STILL frame (decoded once, then paused) — animation plays
 * only on the dedicated anim page, so other pages pay no decoder cost. The
 * player follows the `animbg` setting live and is idempotent; it materialises
 * on the next 200 ms tick.
 *
 * @param page_name screen-manager page id; string literal (matched by identity)
 */
void zmk_anim_bg_attach(lv_obj_t *parent, const char *page_name);

/**
 * Same lifecycle, ungated by the bg setting: the dedicated anim page plays even
 * with backgrounds off. GIF assets only (pause/resume is a gif API).
 */
lv_obj_t *zmk_anim_page_attach(lv_obj_t *parent, const char *page_name, const char *asset);
