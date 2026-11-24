/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Asset-backed animation helpers for the screen pages (backlog a12).
 * Internal to src/display/: boot splash and the clock background are
 * the v1 consumers; the store itself is the public zmk/assets.h seam.
 *
 * All functions must be called on the display work queue (page build).
 */

#pragma once

#include <stdbool.h>

#include <lvgl.h>

/**
 * Create the named GIF centered on @p parent, sized as authored.
 * @retval NULL asset missing/not a GIF/LVGL allocation failed
 */
lv_obj_t *zmk_anim_gif_create(lv_obj_t *parent, const char *name);

/** True if a named asset exists and this build can play it. */
bool zmk_anim_available(const char *name);

/** Create the named GIF *or* Lottie (dispatch on the asset kind). */
lv_obj_t *zmk_anim_create(lv_obj_t *parent, const char *name);

/* Animated background setting (pad10a parity; since the owner pass
 * 2026-11-24 it scopes the background on all pages), persisted at
 * settings "display/anim" key "bg". The a15 RPC or the dev shell
 * (`animbg on|off`) toggles it; screens only read it. */
bool zmk_anim_bg_enabled(void);
void zmk_anim_bg_set(bool on);

/**
 * Register @p parent as the root of bg page @p page_name for the
 * shared background player (a12 owner pass 2026-11-24: backgrounds on
 * ALL pages, one SHARED player re-parented to the visible one — a
 * player per page blows the PSRAM LVGL heap once every page exists,
 * and pages persist after their first visit). The player follows the
 * `animbg on|off` setting live; a page's rebuild just calls this again
 * with the refreshed root. Idempotent; returns nothing to show yet —
 * the player materialises on the next 200 ms tick.
 *
 * @param page_name the screen-manager page this subtree belongs to
 *                  (string literal — registrations match by identity)
 */
void zmk_anim_bg_attach(lv_obj_t *parent, const char *page_name);

/**
 * Same lifecycle, ungated by the bg setting: a dedicated animation
 * page plays even with backgrounds off (that toggle scopes OTHER
 * screens only). GIF assets only (pause/resume is a gif API).
 */
lv_obj_t *zmk_anim_page_attach(lv_obj_t *parent, const char *page_name, const char *asset);
