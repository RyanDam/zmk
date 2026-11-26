/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Studio `pomo` subsystem: config get/set, session control and state
 * events over the pomodoro engine. Everything goes through zmk_pomo_*;
 * the state notification rides the engine's zmk_pomo_state_changed event.
 */

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk_studio, CONFIG_ZMK_STUDIO_LOG_LEVEL);

#include <zmk/events/pomo_state_changed.h>
#include <zmk/pomodoro.h>
#include <zmk/studio/rpc.h>

ZMK_RPC_SUBSYSTEM(pomo)

#define POMO_RESPONSE(type, ...) ZMK_RPC_RESPONSE(pomo, type, __VA_ARGS__)

#define ZMK_RPC_SUBSYSTEM_HANDLER_EX(prefix, request, func_name, _security)                        \
    STRUCT_SECTION_ITERABLE(zmk_rpc_subsystem_handler, prefix##_subsystem_handler_##request) = {   \
        .func = func_name,                                                                         \
        .subsystem_choice = zmk_studio_Request_##prefix##_tag,                                     \
        .request_choice = zmk_##prefix##_Request_##request##_tag,                                  \
        .security = _security,                                                                     \
    };

static zmk_pomo_PomoConfig config_to_proto(const struct zmk_pomo_config *cfg) {
    zmk_pomo_PomoConfig out = zmk_pomo_PomoConfig_init_zero;

    out.sets = cfg->sets;
    out.work_s = cfg->work_s;
    out.rest_s = cfg->rest_s;
    out.noti_mode = cfg->noti_mode;

    return out;
}

zmk_studio_Response pomo_get_config(const zmk_studio_Request *req) {
    return POMO_RESPONSE(get_config, config_to_proto(zmk_pomo_config_get()));
}

zmk_studio_Response pomo_set_config(const zmk_studio_Request *req) {
    const zmk_pomo_PomoConfig *in = &req->subsystem.pomo.request_type.set_config.config;

    struct zmk_pomo_config next = {
        .sets = (uint8_t)in->sets,
        .work_s = (uint16_t)in->work_s,
        .rest_s = (uint16_t)in->rest_s,
        .noti_mode = (uint8_t)in->noti_mode,
    };

    zmk_pomo_config_set(&next); /* clamps + persists + applies live */

    return POMO_RESPONSE(set_config, true);
}

zmk_studio_Response pomo_get_status(const zmk_studio_Request *req) {
    struct zmk_pomo_status st;

    zmk_pomo_get_status(&st);

    zmk_pomo_PomoStatus out = zmk_pomo_PomoStatus_init_zero;

    out.state = st.state;
    out.paused = st.paused;
    out.set_idx = st.set_idx;
    out.sets = st.sets;

    int64_t left = zmk_pomo_remaining_ms();

    out.remaining_ms = left > 0 ? (uint32_t)left : 0;

    return POMO_RESPONSE(get_status, out);
}

zmk_studio_Response pomo_start(const zmk_studio_Request *req) {
    return POMO_RESPONSE(start, zmk_pomo_start() == 0);
}

zmk_studio_Response pomo_pause(const zmk_studio_Request *req) {
    return POMO_RESPONSE(pause, zmk_pomo_pause() == 0);
}

zmk_studio_Response pomo_resume(const zmk_studio_Request *req) {
    return POMO_RESPONSE(resume, zmk_pomo_resume() == 0);
}

zmk_studio_Response pomo_cancel(const zmk_studio_Request *req) {
    return POMO_RESPONSE(cancel, zmk_pomo_cancel() == 0);
}

ZMK_RPC_SUBSYSTEM_HANDLER_EX(pomo, get_config, pomo_get_config, ZMK_STUDIO_RPC_HANDLER_UNSECURED);
ZMK_RPC_SUBSYSTEM_HANDLER_EX(pomo, set_config, pomo_set_config, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER_EX(pomo, get_status, pomo_get_status, ZMK_STUDIO_RPC_HANDLER_UNSECURED);
ZMK_RPC_SUBSYSTEM_HANDLER_EX(pomo, start, pomo_start, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER_EX(pomo, pause, pomo_pause, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER_EX(pomo, resume, pomo_resume, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER_EX(pomo, cancel, pomo_cancel, ZMK_STUDIO_RPC_HANDLER_SECURED);

static int pomo_event_mapper(const zmk_event_t *eh, zmk_studio_Notification *n) {
    const struct zmk_pomo_state_changed *ev = as_zmk_pomo_state_changed(eh);

    if (!ev) {
        return -ENOTSUP;
    }

    zmk_pomo_PomoStateChanged msg = zmk_pomo_PomoStateChanged_init_zero;

    msg.state = ev->state;
    msg.paused = ev->paused;
    msg.set_idx = ev->set_idx;

    *n = ZMK_RPC_NOTIFICATION(pomo, state_changed, msg);
    return 0;
}

ZMK_RPC_EVENT_MAPPER(pomo, pomo_event_mapper, zmk_pomo_state_changed);
