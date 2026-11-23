/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Raised when the live keymap table changes underneath the UI: a runtime
 * binding edit (Studio's keymap RPC / the vkey shell), a settings reload,
 * or a discard. UIs (the tab1a keymap screen, a13) subscribe instead of
 * caching bindings forever; `structural` means layer order/names may have
 * changed too, not just one cell.
 */

#pragma once

#include <zephyr/kernel.h>
#include <zmk/event_manager.h>

struct zmk_keymap_changed {
    bool structural;
};

ZMK_EVENT_DECLARE(zmk_keymap_changed);
