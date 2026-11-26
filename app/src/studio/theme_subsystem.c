/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Studio `theme` subsystem (backlog a14): live get/set of the a07
 * design-system theme (5 RGB888 slots, pad10a 0x64 parity). Writes go
 * through zmk_display_theme_set — the single persist + re-render choke
 * point; the theme_changed notification rides the event that same
 * choke point raises, so preset cycles and settings loads are visible
 * to the host too.
 */

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk_studio, CONFIG_ZMK_STUDIO_LOG_LEVEL);

#include <lvgl.h>

#include <zmk/events/theme_changed.h>
#include <zmk/studio/rpc.h>

#include "../display/theme.h"

ZMK_RPC_SUBSYSTEM(theme)

#define THEME_RESPONSE(type, ...) ZMK_RPC_RESPONSE(theme, type, __VA_ARGS__)

#define ZMK_RPC_SUBSYSTEM_HANDLER_EX(prefix, request, func_name, _security)                        \
    STRUCT_SECTION_ITERABLE(zmk_rpc_subsystem_handler, prefix##_subsystem_handler_##request) = {   \
        .func = func_name,                                                                         \
        .subsystem_choice = zmk_studio_Request_##prefix##_tag,                                     \
        .request_choice = zmk_##prefix##_Request_##request##_tag,                                  \
        .security = _security,                                                                     \
    };

static void rgb_from_color(lv_color_t c, zmk_theme_Rgb *out) {
    out->r = c.red;
    out->g = c.green;
    out->b = c.blue;
}

static lv_color_t rgb_to_color(const zmk_theme_Rgb *in) {
    return lv_color_make((uint8_t)CLAMP(in->r, 0u, 255u), (uint8_t)CLAMP(in->g, 0u, 255u),
                         (uint8_t)CLAMP(in->b, 0u, 255u));
}

static zmk_theme_Theme theme_to_proto(const struct zmk_display_theme *t) {
    zmk_theme_Theme out = zmk_theme_Theme_init_zero;

    rgb_from_color(t->background, &out.background);
    rgb_from_color(t->text_primary, &out.text_primary);
    rgb_from_color(t->text_secondary, &out.text_secondary);
    rgb_from_color(t->accent, &out.accent);
    rgb_from_color(t->inactive, &out.inactive);

    return out;
}

zmk_studio_Response theme_get_theme(const zmk_studio_Request *req) {
    return THEME_RESPONSE(get_theme, theme_to_proto(zmk_display_theme_get()));
}

zmk_studio_Response theme_set_theme(const zmk_studio_Request *req) {
    const zmk_theme_Theme *in = &req->subsystem.theme.request_type.set_theme.theme;

    struct zmk_display_theme next = {
        .background = rgb_to_color(&in->background),
        .text_primary = rgb_to_color(&in->text_primary),
        .text_secondary = rgb_to_color(&in->text_secondary),
        .accent = rgb_to_color(&in->accent),
        .inactive = rgb_to_color(&in->inactive),
    };

    int err = zmk_display_theme_set(&next);

    LOG_INF("theme RPC: set (%s)", err == 0 ? "ok" : "persist failed, live applied");

    return THEME_RESPONSE(set_theme, err == 0);
}

ZMK_RPC_SUBSYSTEM_HANDLER_EX(theme, get_theme, theme_get_theme, ZMK_STUDIO_RPC_HANDLER_UNSECURED);
ZMK_RPC_SUBSYSTEM_HANDLER_EX(theme, set_theme, theme_set_theme, ZMK_STUDIO_RPC_HANDLER_SECURED);

static int theme_event_mapper(const zmk_event_t *eh, zmk_studio_Notification *n) {
    const struct zmk_display_theme_changed *ev = as_zmk_display_theme_changed(eh);

    if (!ev) {
        return -ENOTSUP;
    }

    zmk_theme_Theme msg = zmk_theme_Theme_init_zero;

    msg.background = (zmk_theme_Rgb){.r = (ev->background >> 16) & 0xFF,
                                     .g = (ev->background >> 8) & 0xFF,
                                     .b = ev->background & 0xFF};
    msg.text_primary = (zmk_theme_Rgb){.r = (ev->text_primary >> 16) & 0xFF,
                                       .g = (ev->text_primary >> 8) & 0xFF,
                                       .b = ev->text_primary & 0xFF};
    msg.text_secondary = (zmk_theme_Rgb){.r = (ev->text_secondary >> 16) & 0xFF,
                                         .g = (ev->text_secondary >> 8) & 0xFF,
                                         .b = ev->text_secondary & 0xFF};
    msg.accent = (zmk_theme_Rgb){.r = (ev->accent >> 16) & 0xFF, .g = (ev->accent >> 8) & 0xFF,
                                 .b = ev->accent & 0xFF};
    msg.inactive = (zmk_theme_Rgb){.r = (ev->inactive >> 16) & 0xFF,
                                   .g = (ev->inactive >> 8) & 0xFF, .b = ev->inactive & 0xFF};

    *n = ZMK_RPC_NOTIFICATION(theme, theme_changed, msg);
    return 0;
}

ZMK_RPC_EVENT_MAPPER(theme, theme_event_mapper, zmk_display_theme_changed);
