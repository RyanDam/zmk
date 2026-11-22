/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Coban Tab 1a screen manager (backlog a08): the page system every
 * display-first feature plugs into.
 *
 * A "screen" is a full-page UI (320x480) built through the design system
 * (a07). Screens self-register with ZMK_DISPLAY_DEFINE_SCREEN and are
 * ordered by their `order` field. The manager owns:
 *
 *   - the swipe left/right navigation (touch is the activity signal),
 *   - the page-dot indicator, the boot splash, and the persisted home page,
 *   - the idle backlight policy (full → dim, then blank unless the blank
 *     timeout is 0; wake on touch), and the blank-lock seam later features
 *     (a10 pomodoro, a12 ambient art) use to keep the screen alive, replacing
 *     pad10a's screen_pomodoro_session_running() check in housekeeping_task_user.
 *
 * All LVGL touching happens on the display work queue; the public calls are
 * safe from any thread.
 */

#pragma once

#include <lvgl.h>

#include <zephyr/linker/iterable_sections.h>
#include <zephyr/sys/atomic.h>

/**
 * @brief A screen registered with the manager.
 *
 * @member name   Stable id, used by settings and (later, a14) the Studio RPC.
 * @member create Build the page's content under @p parent and return @p
 *                parent. Runs on the display work queue; the page already
 *                carries the ZMK_STYLE_SCREEN style — screens must style
 *                their content only through zmk_display_style() (a07 rule).
 * @member order  Page order; lower comes first. Ties keep linker order.
 */
struct zmk_display_screen {
    const char *name;
    lv_obj_t *(*create)(lv_obj_t *parent);
    uint8_t order;
};

/**
 * @brief Register a screen with the manager.
 *
 * Exactly one definition per @p sym per image.
 */
#define ZMK_DISPLAY_DEFINE_SCREEN(sym, _name, _create, _order)                                     \
    const STRUCT_SECTION_ITERABLE(zmk_display_screen, zmk_display_screen_##sym) = {                \
        .name = (_name), .create = (_create), .order = (_order),                                   \
    }

/** Number of registered screens. */
int zmk_display_screen_count(void);

/** Registered screen at @p index, or NULL. */
const struct zmk_display_screen *zmk_display_screen_get(int index);

/** Index of the screen registered under @p name, or -1. */
int zmk_display_screen_find(const char *name);

/** Index of the page currently displayed (or the pending target). */
int zmk_display_screen_current(void);

/** Navigate to page @p index. Clamped; safe from any thread. */
int zmk_display_screen_goto(int index);

/** Navigate one page right / left, wrapping at the edges. */
int zmk_display_screen_next(void);
int zmk_display_screen_prev(void);

/** Set (and persist) the page shown after the boot splash. */
int zmk_display_screen_set_home(int index);

/**
 * @brief Suppress the idle dim/blank policy while any lock is held
 * (refcounted). Taking a lock wakes a blanked screen.
 */
void zmk_display_blank_lock(void);
void zmk_display_blank_unlock(void);

/** Wake the backlight to full and restart the idle timer. */
void zmk_display_backlight_wake(void);

/** Called by the display module during display init. */
int zmk_display_screen_mgr_init(void);

/**
 * Start/stop LVGL ticking + panel blanking (wrappers around the display
 * module's blank/unblank work). Declared here because the idle policy needs
 * them; not for general feature use — use the blank lock instead.
 */
void zmk_display_updates_start(void);
void zmk_display_updates_stop(void);
