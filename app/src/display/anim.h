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

/* Animated background behind the clock (pad10a parity), persisted at
 * settings "display/anim" key "bg". The a15 RPC or the dev shell
 * (`animbg on|off`) toggles it; screens only read it. */
bool zmk_anim_bg_enabled(void);
void zmk_anim_bg_set(bool on);
