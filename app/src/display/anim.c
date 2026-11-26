/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Animation playback helpers (backlog a12). See anim.h.
 *
 * GIFs stream through LVGL's lv_fs drive 'A:' mapped onto the asset
 * store, so the decoder reads straight from flash a chunk at a time —
 * pad10a's whole-GIF-into-RAM approach (pad10a/pad10a.c:56-63) is what
 * the streamed store API was designed to avoid. The decoded frame
 * canvas (ARGB, ~600 KB full-screen) lives in the PSRAM LVGL heap; the
 * conf sizes that heap for it.
 *
 * Lottie (ThorVG) decodes whole JSON blobs — small by nature — into a
 * malloc'd buffer; the canvas buffer follows the widget lifetime via
 * the DELETE event.
 */

#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>

#include <lvgl.h>

#include <zmk/assets.h>
#include <zmk/display/screens.h>
#include <zmk/event_manager.h>
#include <zmk/events/assets_changed.h>

#include "anim.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/settings_blob.h>

/* ------------------------------------------------------------------ */
/* LVGL filesystem drive 'A:' over the asset store                     */
/* ------------------------------------------------------------------ */

#define FS_SLOTS 6 /* shared bg + dedicated player + boot splash + slack */

struct fs_file {
    const struct zmk_asset_info *asset;
    uint32_t pos;
    uint32_t epoch; /* store epoch at open; stale handles read as EOF */
};

static struct fs_file fs_slots[FS_SLOTS];
static bool fs_registered;

static void *fs_open_cb(lv_fs_drv_t *drv, const char *path, lv_fs_mode_t mode) {
    ARG_UNUSED(drv);
    ARG_UNUSED(mode);

    if (path == NULL) {
        return NULL;
    }
    while (*path == '/' || *path == ':') {
        path++;
    }

    const struct zmk_asset_info *asset = zmk_asset_find(path);

    if (asset == NULL) {
        return NULL;
    }
    for (int i = 0; i < FS_SLOTS; i++) {
        if (fs_slots[i].asset == NULL) {
            fs_slots[i].asset = asset;
            fs_slots[i].pos = 0;
            fs_slots[i].epoch = zmk_assets_epoch();
            return &fs_slots[i];
        }
    }
    return NULL;
}

static lv_fs_res_t fs_close_cb(lv_fs_drv_t *drv, void *file_p) {
    struct fs_file *f = file_p;

    ARG_UNUSED(drv);
    if (f != NULL) {
        f->asset = NULL;
    }
    return LV_FS_RES_OK;
}

static lv_fs_res_t fs_read_cb(lv_fs_drv_t *drv, void *file_p, void *buf, uint32_t btr,
                              uint32_t *br) {
    struct fs_file *f = file_p;

    ARG_UNUSED(drv);
    *br = 0;
    if (f == NULL || f->asset == NULL) {
        return LV_FS_RES_INV_PARAM;
    }

    /* Generation guard (a15): the store rebuilds its table when an
     * upload/delete commits. The tick closes players before any
     * re-probe, so a handle that sees a new epoch is one the tick has
     * not reaped yet — report EOF (zero bytes, the decoder's stop
     * signal — instead of streaming bytes of a possibly-recycled
     * entry. */
    if (f->epoch != zmk_assets_epoch()) {
        *br = 0;
        return LV_FS_RES_OK;
    }

    uint32_t left = f->asset->len - f->pos;
    uint32_t want = btr < left ? btr : left;

    if (want == 0) {
        return LV_FS_RES_OK; /* EOF: zero bytes read */
    }

    int err = zmk_asset_read(f->asset, f->pos, buf, want);

    if (err != 0) {
        LOG_WRN("asset '%s' read failed at %u (err %d)", f->asset->name, f->pos, err);
        return LV_FS_RES_UNKNOWN;
    }
    f->pos += want;
    *br = want;
    return LV_FS_RES_OK;
}

static lv_fs_res_t fs_seek_cb(lv_fs_drv_t *drv, void *file_p, uint32_t pos, lv_fs_whence_t whence) {
    struct fs_file *f = file_p;

    ARG_UNUSED(drv);
    if (f == NULL || f->asset == NULL) {
        return LV_FS_RES_INV_PARAM;
    }

    /* LVGL's drive API carries seek offsets as uint32_t; a signed
     * interpretation keeps a decoder's peek-backward (negative) seek
     * legal instead of silently rejecting it. */
    int32_t off = (int32_t)pos;
    int32_t target;

    switch (whence) {
    case LV_FS_SEEK_SET:
        target = off;
        break;
    case LV_FS_SEEK_CUR:
        target = (int32_t)f->pos + off;
        break;
    case LV_FS_SEEK_END:
        target = (int32_t)f->asset->len + off;
        break;
    default:
        return LV_FS_RES_INV_PARAM;
    }
    if (target < 0 || target > (int32_t)f->asset->len) {
        return LV_FS_RES_INV_PARAM;
    }
    f->pos = (uint32_t)target;
    return LV_FS_RES_OK;
}

static lv_fs_res_t fs_tell_cb(lv_fs_drv_t *drv, void *file_p, uint32_t *pos_p) {
    struct fs_file *f = file_p;

    ARG_UNUSED(drv);
    if (f == NULL || f->asset == NULL || pos_p == NULL) {
        return LV_FS_RES_INV_PARAM;
    }
    *pos_p = f->pos;
    return LV_FS_RES_OK;
}

static void anim_fs_register(void) {
    static lv_fs_drv_t drv;

    if (zmk_asset_count() == 0) {
        return; /* empty store now; a15 uploads may register later */
    }
    fs_registered = true;
    lv_fs_drv_init(&drv);
    drv.letter = 'A';
    drv.open_cb = fs_open_cb;
    drv.close_cb = fs_close_cb;
    drv.read_cb = fs_read_cb;
    drv.seek_cb = fs_seek_cb;
    /* Mandatory for the GIF decoder: it sizes the file via
     * seek(END)+tell, and lv_fs_tell() silently leaves the size unset
     * without a tell callback -> GIF_EARLY_EOF on every open (the
     * 2026-11-21 v1 missed this because nothing ever reached the
     * glass; a flash-size read is the first thing the decoder does). */
    drv.tell_cb = fs_tell_cb;
    lv_fs_drv_register(&drv);
}

/* ------------------------------------------------------------------ */
/* Players                                                             */
/* ------------------------------------------------------------------ */

lv_obj_t *zmk_anim_gif_create(lv_obj_t *parent, const char *name) {
    const struct zmk_asset_info *asset = zmk_asset_find(name);

    if (asset == NULL || asset->kind != ZMK_ASSET_GIF) {
        return NULL;
    }
    if (!fs_registered) {
        anim_fs_register();
    }

    char path[32];

    snprintk(path, sizeof(path), "A:%s", name);

    lv_obj_t *gif = lv_gif_create(parent);

    /* Render straight into the panel's own depth (RGB565) instead of
     * the widget's ARGB8888 default: halves PSRAM traffic (307 KB vs
     * 614 KB canvas at 480x320, decode writes included) and lets the
     * full-screen blit take the same-format opaque-copy path rather
     * than per-pixel ARGB blending - the dominant per-frame cost on
     * this full-refresh panel (bench lag pass 2026-11-24). 8-bit GIF
     * art means 565 loses nothing visible. */
    lv_gif_set_color_format(gif, LV_COLOR_FORMAT_RGB565);

    lv_gif_set_src(gif, path);

    /* Open is lazy-failing: allocation/decode problems leave an empty
     * image (the bench's "black anim page"). Surface it instead of
     * logging a lie about playing. */
    if (!lv_gif_is_loaded(gif)) {
        LOG_ERR("anim: '%s' failed to open (asset unreadable?)", asset->name);
        lv_obj_del(gif);
        return NULL;
    }

    lv_obj_center(gif);
    lv_obj_remove_flag(gif, LV_OBJ_FLAG_CLICKABLE);

    LOG_INF("anim: gif '%s' open (%u bytes, %u fps spec)", asset->name, asset->len, asset->fps);
    return gif;
}

#if IS_ENABLED(CONFIG_ZMK_DISPLAY_ANIM_LOTTIE)

static void lottie_delete_cb(lv_event_t *e) { lv_free(lv_event_get_user_data(e)); }

static lv_obj_t *lottie_create(lv_obj_t *parent, const struct zmk_asset_info *asset) {
    /* Lottie JSONs are authored small (KBs); one full read is fine. */
    void *json = lv_malloc(asset->len);

    if (json == NULL) {
        LOG_WRN("anim: no room for lottie '%s' (%u bytes)", asset->name, asset->len);
        return NULL;
    }
    if (zmk_asset_read(asset, 0, json, asset->len) != 0) {
        lv_free(json);
        return NULL;
    }

    lv_obj_t *obj = lv_lottie_create(parent);
    /* Landscape stage = the whole logical screen; the ARGB canvas is a
     * 600 KB lv_malloc from the PSRAM pool (was 400 KB for the square
     * 320x320 portrait stage) - see the pool sizing note in cobantab1a.conf. */
    const int32_t w = ZMK_GRID_SCREEN_W, h = ZMK_GRID_SCREEN_H;
    void *canvas = lv_malloc(w * h * 4);

    if (canvas == NULL) {
        lv_obj_del(obj);
        lv_free(json);
        LOG_WRN("anim: no canvas room for lottie '%s'", asset->name);
        return NULL;
    }

    lv_lottie_set_buffer(obj, w, h, canvas);
    lv_lottie_set_src_data(obj, json, asset->len); /* ThorVG parses now */
    lv_free(json);

    lv_obj_center(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(obj, lottie_delete_cb, LV_EVENT_DELETE, canvas);

    LOG_INF("anim: playing lottie '%s' (%u bytes)", asset->name, asset->len);
    return obj;
}

#endif /* CONFIG_ZMK_DISPLAY_ANIM_LOTTIE */

bool zmk_anim_available(const char *name) {
    const struct zmk_asset_info *asset = zmk_asset_find(name);

    if (asset == NULL) {
        return false;
    }
#if !IS_ENABLED(CONFIG_ZMK_DISPLAY_ANIM_LOTTIE)
    return asset->kind == ZMK_ASSET_GIF;
#else
    return true;
#endif
}

lv_obj_t *zmk_anim_create(lv_obj_t *parent, const char *name) {
    const struct zmk_asset_info *asset = zmk_asset_find(name);

    if (asset == NULL) {
        return NULL;
    }
    switch (asset->kind) {
    case ZMK_ASSET_GIF:
        return zmk_anim_gif_create(parent, name);
    case ZMK_ASSET_LOTTIE:
#if IS_ENABLED(CONFIG_ZMK_DISPLAY_ANIM_LOTTIE)
        return lottie_create(parent, asset);
#endif
        return NULL;
    default:
        return NULL;
    }
}

/* ------------------------------------------------------------------ */
/* Page attach: one shared background + dedicated full-page players    */
/* ------------------------------------------------------------------ */

/* A GIF player holds a native-size ARGB canvas (worst case full-screen
 * 480x320 = 614 KB) from the LVGL heap (one 2 MB pool for the whole
 * UI) plus an 'A:' file slot. One player per page exhausts both once every page exists
 * (pages persist
 * after their first visit) and the over-budget pages silently render
 * black (bench 2026-11-24: the anim page was). Backgrounds therefore
 * share ONE player that is re-parented to the visible page; only the
 * dedicated full-page player (the anim page: may show another asset)
 * exists alongside it. At most two players ever. */

#define ANIM_BG_PAGES_MAX 6 /* clock, pomodoro, stats, keymap + 2 */
#define ANIM_SLOTS_MAX 2    /* dedicated players (anim page today) */
#define ANIM_TICK_MS 200

struct anim_bg_page {
    const char *page_name; /* identity compare: callers pass literals */
    lv_obj_t *root;        /* subtree root, refreshed on rebuilds     */
};

static struct anim_bg_page bg_pages[ANIM_BG_PAGES_MAX];
static lv_obj_t *bg_shared; /* NULL = not built / died with its page */

/* Active asset (a14): the Studio RPC's `activate_asset` seam. The 200 ms
 * sync tick owns the LVGL swap; callers only change these. active_asset
 * is what should play, bg_shown what bg_shared currently plays. */
static char active_asset[ZMK_ASSET_NAME_MAX] = "animbg";
static char bg_shown[ZMK_ASSET_NAME_MAX];

struct anim_slot {
    lv_obj_t *gif; /* NULL = free (or the gif died with its page)   */
    const char *page_name;
    /* a15: a re-probe reaps the player (stale) so the store table can
     * rebuild; the tick rebuilds it on the still-live page root. */
    lv_obj_t *parent;
    char want[ZMK_ASSET_NAME_MAX];
    bool stale;
};

static struct anim_slot anim_slots[ANIM_SLOTS_MAX];
static lv_timer_t *anim_tick_timer;

/* LVGL auto-pauses an invisible gif from its own next tick (the timer
 * stays paused, nothing re-checks visibility). Only the dedicated
 * full-page players animate, so the shared timer resumes those when
 * their page shows; the shared background stays paused everywhere
 * (pad10a still-frame rule above). */
static void anim_shared_died(lv_event_t *e) {
    ARG_UNUSED(e);
    bg_shared = NULL;
}

static void anim_slot_died(lv_event_t *e) {
    struct anim_slot *slot = lv_event_get_user_data(e);

    slot->gif = NULL;
    if (!slot->stale) {
        /* The PAGE died (not our re-probe reap): release the slot —
         * a later page rebuild re-attaches into a fresh one. */
        slot->page_name = NULL;
        slot->parent = NULL;
        slot->want[0] = '\0';
    }
}

static lv_obj_t *anim_gif_player(lv_obj_t *parent, const char *asset, lv_event_cb_t died,
                                 void *user) {
    lv_obj_t *gif = zmk_anim_gif_create(parent, asset);

    if (gif == NULL) {
        return NULL;
    }
    lv_gif_set_auto_pause_invisible(gif, true);
    lv_obj_add_event_cb(gif, died, LV_EVENT_DELETE, user);

    return gif;
}

/*
 * Black scrim between the still-frame GIF and the page's widgets
 * (owner 2026-11-24: busy art makes screen content hard to read).
 * Rides the shared player's lifecycle exactly - created, re-parented
 * and deleted together, pinned directly above it (index 1) and below
 * the widgets - and never clickable/scrollable, so touch passes
 * through. Raw LVGL opacity: CONFIG default 51 (= 20%); 0 disables. The
 * 200 ms sync tick is the only writer of the LVGL style, so the
 * shell/settings thread stays out of the display thread's tree.
 */
static lv_obj_t *bg_tint;
static uint8_t anim_tint_opa = CONFIG_ZMK_DISPLAY_ANIM_TINT_OPA;
static uint8_t tint_applied; /* opa the live object currently shows */

static void anim_tint_died(lv_event_t *e) {
    ARG_UNUSED(e);
    bg_tint = NULL;
}

static void anim_tint_sync(lv_obj_t *target) {
    bool on_player;

    if (anim_tint_opa == 0) {
        if (bg_tint != NULL) {
            lv_obj_delete(bg_tint);
        }
        return;
    }

    if (bg_tint == NULL) {
        bg_tint = lv_obj_create(target);
        lv_obj_remove_style_all(bg_tint);
        lv_obj_set_size(bg_tint, lv_pct(100), lv_pct(100));
        lv_obj_set_style_bg_color(bg_tint, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(bg_tint, anim_tint_opa, 0);
        lv_obj_remove_flag(bg_tint, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(bg_tint, anim_tint_died, LV_EVENT_DELETE, NULL);
    } else if (lv_obj_get_parent(bg_tint) != target) {
        lv_obj_set_parent(bg_tint, target);
    }

    if (tint_applied != anim_tint_opa) { /* invalidate only on change */
        lv_obj_set_style_bg_opa(bg_tint, anim_tint_opa, 0);
        tint_applied = anim_tint_opa;
    }

    /* Z-order, explicitly: the page's widgets were built before the
     * scrim arrives, so a fresh child would land ON TOP of them -
     * the same last-child trap as the player (pin to index 0/1). */
    on_player = bg_shared != NULL && lv_obj_get_parent(bg_shared) == target;
    lv_obj_move_to_index(bg_tint, on_player ? 1 : 0);
}

/* pad10a rule (owner 2026-11-24, ported from Pad 10A's behaviour):
 * animation plays ONLY on the dedicated anim page. Everywhere else
 * the shared GIF background is a frozen frame: the widget stays
 * visible under the page's widgets (art without the decoder cost),
 * and nothing about the setting changes - `animbg on` shows the
 * still, `off` removes it. The player decodes one frame after
 * creation, then we pause its timer; a frozen player costs zero
 * decode time and auto-pauses while hidden. */
#define ANIM_BG_FREEZE_MS 300 /* >= one art frame (100-120 ms) + jitter */

static void anim_bg_freeze_cb(lv_timer_t *timer) {
    lv_obj_t *gif = lv_timer_get_user_data(timer);

    /* Identity guard: a player deleted before this fired is gone
     * (bg_shared nulled by the DELETE event); a *freshly recreated*
     * one matching identity gets frozen a frame early at worst,
     * and the next toggle restores the still. */
    if (bg_shared == gif) {
        lv_gif_pause(gif);
    }
}

static void anim_bg_sync(void) {
    lv_obj_t *target = NULL;
    int cur;

    if (!zmk_anim_bg_enabled()) {
        if (bg_shared != NULL) {
            lv_obj_delete(bg_shared); /* clears bg_shared via the cb */
        }
        if (bg_tint != NULL) {
            lv_obj_delete(bg_tint);
        }
        return;
    }

    cur = zmk_display_screen_current();
    for (int i = 0; i < ANIM_BG_PAGES_MAX; i++) {
        if (bg_pages[i].page_name != NULL &&
            zmk_display_screen_find(bg_pages[i].page_name) == cur) {
            target = bg_pages[i].root;
            break;
        }
    }
    if (target == NULL) {
        return; /* e.g. the anim page: park the shared player paused
                 * on its last page (canvas retained, decoder idle) */
    }

    const char *want = zmk_anim_active_get();

    if (bg_shared != NULL && strcmp(bg_shown, want) != 0) {
        /* a14 activate_asset: retire the old player; the next tick
         * (this same function) builds the new art. Display thread
         * only, like everything here. */
        lv_obj_delete(bg_shared);
    }

    if (bg_shared == NULL) {
        bg_shared = anim_gif_player(target, want, anim_shared_died, NULL);
        if (bg_shared != NULL) {
            strncpy(bg_shown, want, sizeof(bg_shown) - 1);
            bg_shown[sizeof(bg_shown) - 1] = '\0';
            /* The page is fully built when the player materialises
             * (creation happens on the tick, not at attach), so the
             * gif arrives as the LAST child - on top of everything,
             * and its parent never "changes" afterwards, so the
             * re-parent path below would never demote it. Pin it to
             * the bottom or the page renders as just the anim frame
             * (bench 2026-11-24). */
            lv_obj_move_to_index(bg_shared, 0);
            lv_timer_t *freeze =
                lv_timer_create(anim_bg_freeze_cb, ANIM_BG_FREEZE_MS, bg_shared);

            lv_timer_set_repeat_count(freeze, 1); /* fires once, self-deletes */
        }
    } else if (lv_obj_get_parent(bg_shared) != target) {
        lv_obj_set_parent(bg_shared, target);
        lv_obj_move_to_index(bg_shared, 0); /* background = bottom */
    }
    /* No resume: backgrounds are still frames off the anim page. */

    anim_tint_sync(target); /* readability scrim, index 1 (see above) */
}

/* ------------------------------------------------------------------ */
/* a15: store-change / quiesce handshake (display-thread side)        */
/* ------------------------------------------------------------------ */

/* Set from the event listener (any thread); consumed here, on the LVGL
 * timer thread — the only place allowed to touch the object tree.
 * QUIESCE parks playback for GC's blob relocation (the store blocks its
 * caller until the ack); a commit (CHANGED) reaps every player, then
 * re-probes the table so no 'A:' handle ever spans a rebuild. */
static volatile bool assets_dirty;
static volatile bool assets_park;
static volatile bool assets_parked;

static int assets_changed_event_cb(const zmk_event_t *eh) {
    const struct zmk_assets_changed *ev = as_zmk_assets_changed(eh);

    if (ev != NULL) {
        if (ev->reason == ZMK_ASSETS_QUIESCE) {
            assets_park = true;
        } else {
            assets_dirty = true;
        }
    }
    return 0;
}

ZMK_LISTENER(anim_assets, assets_changed_event_cb);
ZMK_SUBSCRIPTION(anim_assets, zmk_assets_changed);

static void anim_park_all_players(void) {
    if (bg_shared != NULL) {
        lv_obj_delete(bg_shared); /* the DELETE cb clears bg_shared */
    }
    if (bg_tint != NULL) {
        lv_obj_delete(bg_tint);
    }
    for (int i = 0; i < ANIM_SLOTS_MAX; i++) {
        if (anim_slots[i].gif != NULL) {
            anim_slots[i].stale = true; /* died cb keeps identity */
            lv_obj_delete(anim_slots[i].gif);
        }
    }
    bg_shown[0] = '\0';
}

static void anim_tick_cb(lv_timer_t *timer) {
    int cur = zmk_display_screen_current();

    ARG_UNUSED(timer);

    if (assets_park) {
        assets_park = false;
        anim_park_all_players();
        if (zmk_assets_quiesce_ack()) {
            assets_parked = true;
            return; /* GC owns the blobs until release */
        }
        /* the requester timed out mid-handshake: carry on normally */
        assets_dirty = true;
    }
    if (assets_parked) {
        if (!zmk_assets_quiesced()) {
            assets_parked = false; /* released: fall through to re-probe */
            assets_dirty = true;
        } else {
            return;
        }
    }
    if (assets_dirty) {
        assets_dirty = false;
        anim_park_all_players(); /* no open handle spans the re-probe */
        zmk_assets_reprobe();
    }

    anim_bg_sync();

    for (int i = 0; i < ANIM_SLOTS_MAX; i++) {
        struct anim_slot *slot = &anim_slots[i];

        if (slot->gif != NULL) {
            if (zmk_display_screen_find(slot->page_name) == cur) {
                lv_gif_resume(slot->gif);
            }
            continue;
        }
        if (slot->stale && slot->page_name != NULL &&
            zmk_display_screen_find(slot->page_name) == cur) {
            /* rebuild once (e.g. fresh upload of the played asset);
             * on failure the page's next rebuild re-attaches anyway */
            slot->stale = false;
            slot->gif = anim_gif_player(slot->parent, slot->want, anim_slot_died, slot);
        }
    }
}

/* Slot for a (re)build: the page's own slot first (identity compare —
 * callers pass string literals), else the first unclaimed slot. */
static struct anim_slot *anim_slot_get(const char *page_name) {
    struct anim_slot *free_slot = NULL;

    for (int i = 0; i < ANIM_SLOTS_MAX; i++) {
        if (anim_slots[i].gif != NULL) {
            continue;
        }
        if (anim_slots[i].page_name == page_name) {
            return &anim_slots[i];
        }
        if (free_slot == NULL && anim_slots[i].page_name == NULL) {
            free_slot = &anim_slots[i];
        }
    }
    return free_slot;
}

static void anim_tick_start(void) {
    if (anim_tick_timer == NULL) {
        anim_tick_timer = lv_timer_create(anim_tick_cb, ANIM_TICK_MS, NULL);
    }
}

void zmk_anim_bg_attach(lv_obj_t *parent, const char *page_name) {
    struct anim_bg_page *free_e = NULL;

    for (int i = 0; i < ANIM_BG_PAGES_MAX; i++) {
        if (bg_pages[i].page_name == page_name) {
            bg_pages[i].root = parent; /* rebuild: refresh the root */
            return;
        }
        if (free_e == NULL && bg_pages[i].page_name == NULL) {
            free_e = &bg_pages[i];
        }
    }

    if (free_e == NULL) {
        LOG_WRN("anim: no bg registration for page '%s'", page_name);
        return;
    }
    free_e->page_name = page_name;
    free_e->root = parent;
    anim_tick_start();
}

lv_obj_t *zmk_anim_page_attach(lv_obj_t *parent, const char *page_name, const char *asset) {
    struct anim_slot *slot = anim_slot_get(page_name);

    if (slot == NULL) {
        LOG_WRN("anim: no slot for page '%s'", page_name);
        return NULL;
    }
    slot->page_name = page_name;
    slot->parent = parent;
    slot->stale = false;
    snprintf(slot->want, sizeof(slot->want), "%s", asset);
    slot->gif = anim_gif_player(parent, asset, anim_slot_died, slot);
    anim_tick_start();

    return slot->gif;
}

/* ------------------------------------------------------------------ */
/* Anim-bg setting                                                     */
/* ------------------------------------------------------------------ */

static bool anim_bg = true; /* pad10a default: background on */

bool zmk_anim_bg_enabled(void) { return anim_bg; }

void zmk_anim_bg_set(bool on) {
    anim_bg = on;
#if IS_ENABLED(CONFIG_SETTINGS)
    uint8_t val = on ? 1 : 0;

    zmk_settings_blob_save("display/anim/bg", &val, sizeof(val), "anim bg");
#endif
    LOG_INF("anim bg %s", on ? "on" : "off");
}

const char *zmk_anim_active_get(void) { return active_asset; }

int zmk_anim_active_set(const char *name) {
    if (name == NULL || !zmk_anim_available(name)) {
        return -EINVAL;
    }

    if (strcmp(active_asset, name) == 0) {
        return 0;
    }

    strncpy(active_asset, name, sizeof(active_asset) - 1);
    active_asset[sizeof(active_asset) - 1] = '\0';
#if IS_ENABLED(CONFIG_SETTINGS)
    zmk_settings_blob_save("display/anim/active", active_asset, sizeof(active_asset),
                           "anim active asset");
#endif
    LOG_INF("anim active asset -> %s", active_asset);
    /* shared background re-picks it on the next 200 ms sync tick */
    return 0;
}

uint8_t zmk_anim_tint_get(void) { return anim_tint_opa; }

void zmk_anim_tint_set(uint8_t opa) {
    anim_tint_opa = opa;
#if IS_ENABLED(CONFIG_SETTINGS)
    zmk_settings_blob_save("display/anim/tint", &opa, sizeof(opa), "anim tint");
#endif
    LOG_INF("anim tint: opa %u%s", opa, opa == 0 ? " (off)" : "");
    /* LVGL style applied by the next 200 ms sync tick: the display
     * thread owns the object tree, the shell thread does not. */
}

#if IS_ENABLED(CONFIG_SETTINGS)
static int anim_settings_handle_set(const char *key, size_t len, settings_read_cb read_cb,
                                    void *cb_arg) {
    uint8_t val;
    int err = zmk_settings_blob_read(key, len, read_cb, cb_arg, "bg", &val, sizeof(val), "anim bg");

    if (err != 0) {
        if (err < 0) {
            return err;
        }
        anim_bg = val != 0;
        LOG_INF("anim bg setting loaded: %s", anim_bg ? "on" : "off");
        return 0;
    }

    err = zmk_settings_blob_read(key, len, read_cb, cb_arg, "tint", &val, sizeof(val),
                                 "anim tint");
    if (err > 0) {
        anim_tint_opa = val;
        LOG_INF("anim tint setting loaded: opa %u", val);
        return 0;
    }

    err = zmk_settings_blob_read(key, len, read_cb, cb_arg, "active", active_asset,
                                 sizeof(active_asset), "anim active asset");
    if (err > 0) {
        active_asset[sizeof(active_asset) - 1] = '\0';
        LOG_INF("anim active asset loaded: %s", active_asset);
        return 0;
    }
    return err;
}

SETTINGS_STATIC_HANDLER_DEFINE(display_anim, "display/anim", NULL, anim_settings_handle_set, NULL,
                               NULL);
#endif /* CONFIG_SETTINGS */

/* ------------------------------------------------------------------ */
/* Dev shell (bench toggle without a host)                             */
/* ------------------------------------------------------------------ */

#if IS_ENABLED(CONFIG_SHELL)

#include <zephyr/shell/shell.h>

static int cmd_animbg(const struct shell *sh, size_t argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "on") == 0) {
        zmk_anim_bg_set(true);
    } else if (argc == 2 && strcmp(argv[1], "off") == 0) {
        zmk_anim_bg_set(false);
    } else if (argc == 2 && strcmp(argv[1], "list") == 0) {
        shell_print(sh, "assets: %d", zmk_asset_count());
        for (int i = 0; i < zmk_asset_count(); i++) {
            const struct zmk_asset_info *a = zmk_asset_get(i);

            shell_print(sh, "  %-16s kind=%u len=%u fps=%u", a->name, a->kind, a->len, a->fps);
        }
    } else if (argc >= 2 && strcmp(argv[1], "tint") == 0) {
        if (argc == 3) {
            char *end;
            long v = strtol(argv[2], &end, 10);

            if (*end != '\0' || v < 0 || v > 255) {
                shell_error(sh, "tint opa must be 0-255");
                return -EINVAL;
            }
            zmk_anim_tint_set((uint8_t)v);
        }
        shell_print(sh, "anim tint opa: %u", zmk_anim_tint_get());
    } else if (argc >= 2 && strcmp(argv[1], "use") == 0) {
        if (argc == 3) {
            if (zmk_anim_active_set(argv[2]) != 0) {
                shell_error(sh, "no GIF asset '%s'", argv[2]);
            }
        }
        shell_print(sh, "anim active asset: %s", zmk_anim_active_get());
    } else {
        shell_print(sh, "anim bg: %s, active: %s (usage: animbg on|off|list|tint [0-255]|use <name>)",
                    zmk_anim_bg_enabled() ? "on" : "off", zmk_anim_active_get());
    }
    return 0;
}

SHELL_CMD_REGISTER(animbg, NULL, "Animated background toggle + asset list + tint (a12)",
                   cmd_animbg);

#endif /* CONFIG_SHELL */
