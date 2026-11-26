/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Studio `screen` subsystem (backlog a14): list/navigate the tab's
 * screen manager (a08) and sync the wall clock (a09 seam). Port of
 * pad10a's raw-HID 0x51 set_screen / 0x52 set_time.
 *
 * Seam purity is the a14 rule: this file talks only to
 * zmk_display_screen_* and zmk_time_*, never to a screen's widgets.
 */

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk_studio, CONFIG_ZMK_STUDIO_LOG_LEVEL);

#include <string.h>

#include <pb_encode.h>

#include <zmk/display/screens.h>
#include <zmk/display/time_src.h>
#include <zmk/events/screen_changed.h>
#include <zmk/studio/rpc.h>

ZMK_RPC_SUBSYSTEM(screen)

#define SCREEN_RESPONSE(type, ...) ZMK_RPC_RESPONSE(screen, type, __VA_ARGS__)

#define ZMK_RPC_SUBSYSTEM_HANDLER_EX(prefix, request, func_name, _security)                        \
    STRUCT_SECTION_ITERABLE(zmk_rpc_subsystem_handler, prefix##_subsystem_handler_##request) = {   \
        .func = func_name,                                                                         \
        .subsystem_choice = zmk_studio_Request_##prefix##_tag,                                     \
        .request_choice = zmk_##prefix##_Request_##request##_tag,                                  \
        .security = _security,                                                                     \
    };

static bool encode_screen_list(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    for (int i = 0; i < zmk_display_screen_count(); i++) {
        const struct zmk_display_screen *s = zmk_display_screen_get(i);

        if (s == NULL) {
            continue;
        }

        zmk_screen_ScreenInfo info = zmk_screen_ScreenInfo_init_zero;

        info.order = s->order;
        snprintk(info.name, sizeof(info.name), "%s", s->name);

        if (!pb_encode_tag_for_field(stream, field)) {
            return false;
        }
        if (!pb_encode_submessage(stream, &zmk_screen_ScreenInfo_msg, &info)) {
            return false;
        }
    }
    return true;
}

zmk_studio_Response screen_get_screens(const zmk_studio_Request *req) {
    zmk_screen_GetScreensResponse resp = zmk_screen_GetScreensResponse_init_zero;

    resp.screens.funcs.encode = encode_screen_list;

    int cur = zmk_display_screen_current();
    const struct zmk_display_screen *s = zmk_display_screen_get(cur);

    if (s != NULL) {
        snprintk(resp.current, sizeof(resp.current), "%s", s->name);
    }

    return SCREEN_RESPONSE(get_screens, resp);
}

/* Transient UI navigation (pad10a parity: 0x51 needed no unlock), which
 * is also what makes the harness selftest runnable before core.lock. */
zmk_studio_Response screen_set_screen(const zmk_studio_Request *req) {
    const char *name = req->subsystem.screen.request_type.set_screen.name;
    int idx = zmk_display_screen_find(name);

    if (idx < 0) {
        LOG_WRN("screen RPC: unknown screen \"%s\"", name);
        return ZMK_RPC_SIMPLE_ERR(GENERIC);
    }

    int err = zmk_display_screen_goto(idx);

    return SCREEN_RESPONSE(set_screen, err == 0);
}

/* a09 seam: zmk_time_sync confirms the wall clock (sim until h02 gives
 * the DS1302 a driver; the RPC works either way). */
zmk_studio_Response screen_set_time(const zmk_studio_Request *req) {
    uint32_t epoch_s = req->subsystem.screen.request_type.set_time.epoch_s;

    zmk_time_sync((int64_t)epoch_s);
    LOG_INF("screen RPC: time synced to epoch %u s", epoch_s);

    return SCREEN_RESPONSE(set_time, true);
}

zmk_studio_Response screen_get_time(const zmk_studio_Request *req) {
    bool synced = false;
    int64_t epoch = zmk_time_now_epoch(&synced);

    zmk_screen_GetTimeResponse resp = zmk_screen_GetTimeResponse_init_zero;

    resp.epoch_s = epoch > 0 ? (uint32_t)epoch : 0;
    resp.synced = synced;

    return SCREEN_RESPONSE(get_time, resp);
}

ZMK_RPC_SUBSYSTEM_HANDLER_EX(screen, get_screens, screen_get_screens,
                             ZMK_STUDIO_RPC_HANDLER_UNSECURED);
ZMK_RPC_SUBSYSTEM_HANDLER_EX(screen, set_screen, screen_set_screen,
                             ZMK_STUDIO_RPC_HANDLER_UNSECURED);
ZMK_RPC_SUBSYSTEM_HANDLER_EX(screen, set_time, screen_set_time, ZMK_STUDIO_RPC_HANDLER_UNSECURED);
ZMK_RPC_SUBSYSTEM_HANDLER_EX(screen, get_time, screen_get_time, ZMK_STUDIO_RPC_HANDLER_UNSECURED);

static int screen_event_mapper(const zmk_event_t *eh, zmk_studio_Notification *n) {
    const struct zmk_display_screen_changed *ev = as_zmk_display_screen_changed(eh);

    if (!ev) {
        return -ENOTSUP;
    }

    zmk_screen_ScreenChanged msg = zmk_screen_ScreenChanged_init_zero;

    msg.index = ev->index;
    strncpy(msg.name, ev->name, sizeof(msg.name) - 1);

    *n = ZMK_RPC_NOTIFICATION(screen, screen_changed, msg);
    return 0;
}

ZMK_RPC_EVENT_MAPPER(screen, screen_event_mapper, zmk_display_screen_changed);
