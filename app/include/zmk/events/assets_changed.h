/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Asset store change event (backlog a15). Raised by the store after a
 * manifest commit (upload end, delete, GC) so the display's anim layer
 * can drop its 'A:' players and re-probe on its own thread; and by the
 * quiesce handshake so the tick can park playback for GC.
 */

#pragma once

#include <zmk/event_manager.h>

struct zmk_assets_changed {
    uint8_t reason; /* enum zmk_assets_change_reason */
};

enum zmk_assets_change_reason {
    ZMK_ASSETS_CHANGED = 0, /* new manifest generation is live     */
    ZMK_ASSETS_QUIESCE = 1, /* close all players and ack (for GC)  */
};

ZMK_EVENT_DECLARE(zmk_assets_changed);
