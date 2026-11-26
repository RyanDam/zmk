/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Studio `assets` subsystem (backlog a14 list/activate, a15 upload +
 * delete): manifest list, activate, chunked upload and manifest
 * delete over the a12/a15 asset store and the anim layer's
 * active-asset seam. This file maps proto <-> the plain-C seams
 * (zmk_anim_*, asset_upload.h) and encodes — it never touches flash
 * or store internals directly (the a14 seam rule).
 */

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk_studio, CONFIG_ZMK_STUDIO_LOG_LEVEL);

#include <pb_encode.h>

#include <zmk/assets.h>
#include <zmk/studio/rpc.h>

#if IS_ENABLED(CONFIG_ZMK_STUDIO_ASSET_UPLOAD)
#include "asset_upload.h"
#endif

#if IS_ENABLED(CONFIG_ZMK_DISPLAY_ANIM)
#include "../display/anim.h"
#endif

ZMK_RPC_SUBSYSTEM(assets)

#define ASSETS_RESPONSE(type, ...) ZMK_RPC_RESPONSE(assets, type, __VA_ARGS__)

#define ZMK_RPC_SUBSYSTEM_HANDLER_EX(prefix, request, func_name, _security)                        \
    STRUCT_SECTION_ITERABLE(zmk_rpc_subsystem_handler, prefix##_subsystem_handler_##request) = {   \
        .func = func_name,                                                                         \
        .subsystem_choice = zmk_studio_Request_##prefix##_tag,                                     \
        .request_choice = zmk_##prefix##_Request_##request##_tag,                                  \
        .security = _security,                                                                     \
    };

static bool encode_asset_list(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    for (int i = 0; i < zmk_asset_count(); i++) {
        const struct zmk_asset_info *a = zmk_asset_get(i);

        if (a == NULL) {
            continue;
        }

        zmk_assets_AssetInfo info = zmk_assets_AssetInfo_init_zero;

        info.kind = a->kind;
        info.fps = a->fps;
        info.len = a->len;
        snprintk(info.name, sizeof(info.name), "%s", a->name);

        if (!pb_encode_tag_for_field(stream, field)) {
            return false;
        }
        if (!pb_encode_submessage(stream, &zmk_assets_AssetInfo_msg, &info)) {
            return false;
        }
    }
    return true;
}

zmk_studio_Response assets_list_assets(const zmk_studio_Request *req) {
    zmk_assets_ListAssetsResponse resp = zmk_assets_ListAssetsResponse_init_zero;

    resp.assets.funcs.encode = encode_asset_list;
    resp.ready = zmk_asset_ready();
    resp.generation = zmk_assets_generation(); /* a15: 0 = factory image */
    (void)zmk_assets_free_bytes(&resp.free_bytes);
#if IS_ENABLED(CONFIG_ZMK_DISPLAY_ANIM)
    snprintk(resp.active, sizeof(resp.active), "%s", zmk_anim_active_get());
#endif

    return ASSETS_RESPONSE(list_assets, resp);
}

zmk_studio_Response assets_activate_asset(const zmk_studio_Request *req) {
#if IS_ENABLED(CONFIG_ZMK_DISPLAY_ANIM)
    const char *name = req->subsystem.assets.request_type.activate_asset.name;
    int err = zmk_anim_active_set(name);

    if (err != 0) {
        LOG_WRN("assets RPC: cannot activate \"%s\" (not an available GIF)", name);
        return ZMK_RPC_SIMPLE_ERR(GENERIC);
    }

    return ASSETS_RESPONSE(activate_asset, true);
#else
    return ZMK_RPC_SIMPLE_ERR(GENERIC);
#endif
}

zmk_studio_Response assets_delete_asset(const zmk_studio_Request *req) {
#if IS_ENABLED(CONFIG_ZMK_STUDIO_ASSET_UPLOAD)
    const char *name = req->subsystem.assets.request_type.delete_asset.name;
    bool deleted = false;
    int err = zmk_asset_store_delete(name, &deleted);

    if (err != 0) {
        LOG_WRN("assets RPC: delete \"%s\" failed (err %d)", name, err);
        return ZMK_RPC_SIMPLE_ERR(GENERIC);
    }
    if (!deleted) {
        /* Not in the manifest: a built-in (firmware art, not
         * deletable) or an unknown name. The commit-last design means
         * a built-in name that was once overridden is simply gone with
         * its entry, so "was builtin-only" == "nothing to delete". */
        LOG_INF("assets RPC: delete \"%s\" - no manifest entry", name);
        return ZMK_RPC_SIMPLE_ERR(GENERIC);
    }
    LOG_INF("assets RPC: deleted \"%s\"", name);
    return ASSETS_RESPONSE(delete_asset, true);
#else
    return ZMK_RPC_SIMPLE_ERR(GENERIC);
#endif
}

/* ---- a15: chunked upload ------------------------------------------- *
 * Thin maps onto asset_upload.h (SECURED writes; one session at a
 * time; the handler never blocks on flash — see asset_upload.c).       */

#if IS_ENABLED(CONFIG_ZMK_STUDIO_ASSET_UPLOAD)

zmk_studio_Response assets_upload_begin(const zmk_studio_Request *req) {
    const zmk_assets_UploadBeginRequest *b = &req->subsystem.assets.request_type.upload_begin;
    zmk_assets_UploadBeginResponse resp = zmk_assets_UploadBeginResponse_init_zero;
    const char *err;

    int rc = zmk_asset_upload_begin(b->name, b->size, b->crc32, (uint16_t)b->kind,
                                    (uint16_t)b->fps, &resp.free_bytes, &err);

    resp.accepted = rc == 0;
    if (err != NULL) {
        snprintk(resp.error, sizeof(resp.error), "%s", err);
    }

    return ASSETS_RESPONSE(upload_begin, resp);
}

zmk_studio_Response assets_upload_data(const zmk_studio_Request *req) {
    const zmk_assets_UploadDataRequest *d = &req->subsystem.assets.request_type.upload_data;
    zmk_assets_UploadDataResponse resp = zmk_assets_UploadDataResponse_init_zero;
    const char *err;

    int rc = zmk_asset_upload_data(d->seq, (const uint8_t *)d->data.bytes, d->data.size,
                                   &resp.accepted, &err);

    if (rc != 0 && err != NULL) {
        snprintk(resp.error, sizeof(resp.error), "%s", err);
    }

    return ASSETS_RESPONSE(upload_data, resp);
}

zmk_studio_Response assets_upload_end(const zmk_studio_Request *req) {
    zmk_assets_UploadEndResponse resp = zmk_assets_UploadEndResponse_init_zero;
    const char *err;

    int rc = zmk_asset_upload_end(&resp.free_bytes, &resp.committed, &err);

    if (err != NULL) {
        snprintk(resp.error, sizeof(resp.error), "%s", err);
    }
    if (rc != 0 && !resp.committed && resp.error[0] == '\0') {
        snprintk(resp.error, sizeof(resp.error), "upload end failed (%d)", rc);
    }

    return ASSETS_RESPONSE(upload_end, resp);
}

zmk_studio_Response assets_upload_abort(const zmk_studio_Request *req) {
    bool had_session = zmk_asset_upload_abort() == 0;

    return ASSETS_RESPONSE(upload_abort, had_session);
}

zmk_studio_Response assets_upload_status(const zmk_studio_Request *req) {
    zmk_assets_UploadStatusResponse resp = zmk_assets_UploadStatusResponse_init_zero;

    (void)zmk_asset_upload_status(&resp.active, resp.name, sizeof(resp.name), &resp.accepted);

    return ASSETS_RESPONSE(upload_status, resp);
}

zmk_studio_Response assets_upload_gc(const zmk_studio_Request *req) {
    zmk_assets_UploadGcResponse resp = zmk_assets_UploadGcResponse_init_zero;

    int rc = zmk_asset_upload_gc(&resp.free_bytes);

    resp.ran = rc == 0;

    return ASSETS_RESPONSE(upload_gc, resp);
}

#endif /* CONFIG_ZMK_STUDIO_ASSET_UPLOAD */

ZMK_RPC_SUBSYSTEM_HANDLER_EX(assets, list_assets, assets_list_assets,
                             ZMK_STUDIO_RPC_HANDLER_UNSECURED);
ZMK_RPC_SUBSYSTEM_HANDLER_EX(assets, activate_asset, assets_activate_asset,
                             ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER_EX(assets, delete_asset, assets_delete_asset,
                             ZMK_STUDIO_RPC_HANDLER_SECURED);
#if IS_ENABLED(CONFIG_ZMK_STUDIO_ASSET_UPLOAD)
ZMK_RPC_SUBSYSTEM_HANDLER_EX(assets, upload_begin, assets_upload_begin,
                             ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER_EX(assets, upload_data, assets_upload_data,
                             ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER_EX(assets, upload_end, assets_upload_end,
                             ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER_EX(assets, upload_abort, assets_upload_abort,
                             ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER_EX(assets, upload_status, assets_upload_status,
                             ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER_EX(assets, upload_gc, assets_upload_gc, ZMK_STUDIO_RPC_HANDLER_SECURED);
#endif
