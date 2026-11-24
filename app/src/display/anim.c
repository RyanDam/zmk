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

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>

#include <lvgl.h>

#include <zmk/assets.h>
#include <zmk/display/screens.h>

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

    fs_registered = true; /* once: no retry spam on empty stores */
    if (zmk_asset_count() == 0) {
        return;
    }

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

    LOG_INF("anim: playing gif '%s' (%u bytes, %u fps spec)", asset->name, asset->len, asset->fps);
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

struct anim_slot {
    lv_obj_t *gif; /* NULL = free (or the gif died with its page)   */
    const char *page_name;
};

static struct anim_slot anim_slots[ANIM_SLOTS_MAX];
static lv_timer_t *anim_tick_timer;

/* LVGL auto-pauses an invisible gif from its own next tick (the timer
 * stays paused, nothing re-checks visibility); resume belongs to us.
 * One shared timer walks the registrations — resuming an
 * already-running gif is a no-op, and hidden players never decode. */
static void anim_shared_died(lv_event_t *e) {
    ARG_UNUSED(e);
    bg_shared = NULL;
}

static void anim_slot_died(lv_event_t *e) {
    struct anim_slot *slot = lv_event_get_user_data(e);

    slot->gif = NULL;
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

static void anim_bg_sync(void) {
    lv_obj_t *target = NULL;
    int cur;

    if (!zmk_anim_bg_enabled()) {
        if (bg_shared != NULL) {
            lv_obj_delete(bg_shared); /* clears bg_shared via the cb */
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

    if (bg_shared == NULL) {
        bg_shared = anim_gif_player(target, "animbg", anim_shared_died, NULL);
        return;
    }
    if (lv_obj_get_parent(bg_shared) != target) {
        lv_obj_set_parent(bg_shared, target);
        lv_obj_move_to_index(bg_shared, 0); /* background = bottom */
    }
    lv_gif_resume(bg_shared);
}

static void anim_tick_cb(lv_timer_t *timer) {
    int cur = zmk_display_screen_current();

    ARG_UNUSED(timer);

    anim_bg_sync();

    for (int i = 0; i < ANIM_SLOTS_MAX; i++) {
        if (anim_slots[i].gif != NULL &&
            zmk_display_screen_find(anim_slots[i].page_name) == cur) {
            lv_gif_resume(anim_slots[i].gif);
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

#if IS_ENABLED(CONFIG_SETTINGS)
static int anim_settings_handle_set(const char *key, size_t len, settings_read_cb read_cb,
                                    void *cb_arg) {
    uint8_t val;
    int err = zmk_settings_blob_read(key, len, read_cb, cb_arg, "bg", &val, sizeof(val), "anim bg");

    if (err <= 0) {
        return err;
    }
    anim_bg = val != 0;
    LOG_INF("anim bg setting loaded: %s", anim_bg ? "on" : "off");
    return 0;
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
    } else {
        shell_print(sh, "anim bg: %s (usage: animbg on|off|list)",
                    zmk_anim_bg_enabled() ? "on" : "off");
    }
    return 0;
}

SHELL_CMD_REGISTER(animbg, NULL, "Animated background toggle + asset list (a12)", cmd_animbg);

#endif /* CONFIG_SHELL */
