/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Studio `stats` subsystem: host-pushed machine vitals onto the stats feed
 * seam. Data frames: zmk_stats_update() only (transient, no persist);
 * config: zmk_stats_config_set() (persists + raises the mapped event, so
 * on-screen tap edits reach the host too).
 */

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk_studio, CONFIG_ZMK_STUDIO_LOG_LEVEL);

#include <zmk/events/stats_config_changed.h>
#include <zmk/stats.h>
#include <zmk/studio/rpc.h>

ZMK_RPC_SUBSYSTEM(stats)

#define STATS_RESPONSE(type, ...) ZMK_RPC_RESPONSE(stats, type, __VA_ARGS__)

#define ZMK_RPC_SUBSYSTEM_HANDLER_EX(prefix, request, func_name, _security)                        \
    STRUCT_SECTION_ITERABLE(zmk_rpc_subsystem_handler, prefix##_subsystem_handler_##request) = {   \
        .func = func_name,                                                                         \
        .subsystem_choice = zmk_studio_Request_##prefix##_tag,                                     \
        .request_choice = zmk_##prefix##_Request_##request##_tag,                                  \
        .security = _security,                                                                     \
    };

static zmk_stats_StatsConfig config_to_proto(const struct zmk_stats_config *cfg) {
    zmk_stats_StatsConfig out = zmk_stats_StatsConfig_init_zero;

    out.layout = cfg->layout;
    out.metric_ids_count = ZMK_STATS_SLOT_COUNT;
    for (int i = 0; i < ZMK_STATS_SLOT_COUNT; i++) {
        out.metric_ids[i] = cfg->metric_ids[i];
    }

    return out;
}

zmk_studio_Response stats_get_stats_config(const zmk_studio_Request *req) {
    return STATS_RESPONSE(get_stats_config, config_to_proto(zmk_stats_config_get()));
}

zmk_studio_Response stats_set_stats_config(const zmk_studio_Request *req) {
    const zmk_stats_StatsConfig *in = &req->subsystem.stats.request_type.set_stats_config.config;

    struct zmk_stats_config next = {
        .layout = (uint8_t)in->layout,
    };

    for (int i = 0; i < ZMK_STATS_SLOT_COUNT; i++) {
        next.metric_ids[i] =
            (uint8_t)(i < (int)in->metric_ids_count ? in->metric_ids[i] : ZMK_STATS_NONE);
    }

    int err = zmk_stats_config_set(&next); /* clamp + persist + event */

    return STATS_RESPONSE(set_stats_config, err == 0);
}

/* Live data frames: cheap, nothing persisted — hence UNSECURED; the
 * bare-ack reply lets the host pace itself. */
zmk_studio_Response stats_push_stats_data(const zmk_studio_Request *req) {
    const zmk_stats_PushStatsDataRequest *push =
        &req->subsystem.stats.request_type.push_stats_data;

    for (size_t i = 0; i < push->values_count; i++) {
        const zmk_stats_StatValue *v = &push->values[i];

        zmk_stats_update((uint8_t)v->id, (uint16_t)v->value, (uint16_t)v->max_value,
                         (uint8_t)v->unit_id);
    }

    return STATS_RESPONSE(push_stats_data, true);
}

ZMK_RPC_SUBSYSTEM_HANDLER_EX(stats, get_stats_config, stats_get_stats_config,
                             ZMK_STUDIO_RPC_HANDLER_UNSECURED);
ZMK_RPC_SUBSYSTEM_HANDLER_EX(stats, set_stats_config, stats_set_stats_config,
                             ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER_EX(stats, push_stats_data, stats_push_stats_data,
                             ZMK_STUDIO_RPC_HANDLER_UNSECURED);

static int stats_event_mapper(const zmk_event_t *eh, zmk_studio_Notification *n) {
    const struct zmk_stats_config_changed *ev = as_zmk_stats_config_changed(eh);

    if (!ev) {
        return -ENOTSUP;
    }

    *n = ZMK_RPC_NOTIFICATION(stats, stats_config_changed,
                              config_to_proto(zmk_stats_config_get()));
    return 0;
}

ZMK_RPC_EVENT_MAPPER(stats, stats_event_mapper, zmk_stats_config_changed);
