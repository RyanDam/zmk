/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Coban Tab 1a virtual key matrix model (backlog a13).
 *
 * Pad 10A's layer screen was a read-only picture of the keymap. On tab1a
 * it becomes a soft macropad: a grid of cells that each hold a ZMK
 * behavior binding, fired through zmk_behavior_invoke_binding() exactly
 * like a hardware key would (the phantom 1x1 matrix in the keymap stays
 * untouched — the keymap machinery never knows the difference).
 *
 * The model deliberately lives outside src/display/: a14's Studio RPC
 * reads and edits it over the same API the screen and the dev shell do,
 * with no LVGL involved. Cells carry the binding as {behavior device
 * name, param1, param2} — the serialisable half of struct
 * zmk_behavior_binding — so a14 can assign any behavior, not only
 * dynamic macros. v1 defaults wire the 16 fork dynamic macros
 * (&dm 0..15, settings-persisted) to layer 0.
 *
 * The icon slot named in the backlog is reserved for a16 key art: the
 * cell name string doubles as the icon fallback text.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define ZMK_VKEYMAP_COLS 4
#define ZMK_VKEYMAP_ROWS 4
#define ZMK_VKEYMAP_CELLS (ZMK_VKEYMAP_COLS * ZMK_VKEYMAP_ROWS)
#define ZMK_VKEYMAP_LAYERS 3
#define ZMK_VKEYMAP_NAME_MAX 8
#define ZMK_VKEYMAP_BEHAVIOR_MAX 20

/** One grid cell. behavior[0] == '\0' marks an unassigned (dead) cell. */
struct zmk_vkeymap_cell {
    char name[ZMK_VKEYMAP_NAME_MAX];
    char behavior[ZMK_VKEYMAP_BEHAVIOR_MAX];
    uint32_t param1;
    uint32_t param2;
};

/** Number of soft layers in the model (fixed today, ZMK_VKEYMAP_LAYERS). */
int zmk_vkeymap_layer_count(void);

/** Soft-layer display name (never NULL). */
const char *zmk_vkeymap_layer_name(unsigned int layer);

/** Model cell (row-major, idx < ZMK_VKEYMAP_CELLS); NULL if out of range.
 *  The pointer targets static storage, valid until the next cell change. */
const struct zmk_vkeymap_cell *zmk_vkeymap_cell_get(unsigned int layer, unsigned int idx);

/** Set one cell (name and/or behavior) and persist; notifies listeners.
 *  Assigning a behavior requires it to be a known ZMK behavior device. */
int zmk_vkeymap_cell_set(unsigned int layer, unsigned int idx, const struct zmk_vkeymap_cell *cell);

/** Clear a cell back to dead. Persists; notifies listeners. */
int zmk_vkeymap_cell_clear(unsigned int layer, unsigned int idx);

/**
 * Fire a cell like a key would: invoke the binding pressed, then
 * released ~30 ms later, on the caller's context + a system work.
 * @retval -EINVAL unassigned cell or unknown behavior
 */
int zmk_vkeymap_fire(unsigned int layer, unsigned int idx);

/** Change callback (any thread; take the display queue to touch LVGL). */
typedef void (*zmk_vkeymap_changed_cb)(void);

/** @retval -ENOSPC listener table full (4 slots) */
int zmk_vkeymap_add_listener(zmk_vkeymap_changed_cb cb);
