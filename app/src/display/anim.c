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

#include "anim.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/settings_blob.h>

/* ------------------------------------------------------------------ */
/* LVGL filesystem drive 'A:' over the asset store                     */
/* ------------------------------------------------------------------ */

#define FS_SLOTS 4

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

    uint32_t target;

    switch (whence) {
    case LV_FS_SEEK_SET:
        target = pos;
        break;
    case LV_FS_SEEK_CUR:
        if (pos > f->asset->len || f->pos > f->asset->len - pos) {
            return LV_FS_RES_INV_PARAM;
        }
        target = f->pos + pos;
        break;
    case LV_FS_SEEK_END:
        if (pos > f->asset->len) {
            return LV_FS_RES_INV_PARAM;
        }
        target = f->asset->len - pos;
        break;
    default:
        return LV_FS_RES_INV_PARAM;
    }
    if (target > f->asset->len) {
        return LV_FS_RES_INV_PARAM;
    }
    f->pos = target;
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

    lv_gif_set_src(gif, path);
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
