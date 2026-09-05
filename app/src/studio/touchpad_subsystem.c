/*
 * Copyright (c) 2024 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk_studio, CONFIG_ZMK_STUDIO_LOG_LEVEL);

#include <string.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/keymap.h>
#include <zmk/touchpad.h>
#include <zmk/studio/rpc.h>
#include <pb_encode.h>

ZMK_RPC_SUBSYSTEM(touchpad)

#define TP_RESPONSE(type, ...) ZMK_RPC_RESPONSE(touchpad, type, __VA_ARGS__)
#define TP_NOTIFICATION(type, ...) ZMK_RPC_NOTIFICATION(touchpad, type, __VA_ARGS__)
#define KEYMAP_NOTIFICATION(type, ...) ZMK_RPC_NOTIFICATION(keymap, type, __VA_ARGS__)

static bool encode_layers(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    uint8_t seen_ids = 0;

    for (zmk_keymap_layer_index_t lidx = 0; lidx < ZMK_KEYMAP_LAYERS_LEN; lidx++) {
        zmk_keymap_layer_id_t lid = zmk_keymap_layer_index_to_id(lidx);
        if (lid == ZMK_KEYMAP_LAYER_ID_INVAL || (seen_ids & (1 << lid))) {
            continue;
        }
        seen_ids |= (1 << lid);

        // LOG_WRN("encode_layer idx=%d id=%d", lidx, lid);

        zmk_touchpad_Layer layer_msg = zmk_touchpad_Layer_init_zero;
        layer_msg.id = lid;

        const char *name = zmk_keymap_layer_name(lid);
        if (name) {
            strncpy(layer_msg.name, name, sizeof(layer_msg.name) - 1);
            // LOG_WRN("  name=%s", layer_msg.name);
        }

        layer_msg.mode = zmk_touchpad_get_mode(lidx) == TOUCHPAD_MODE_MOUSE_SIMULATION
                             ? zmk_touchpad_Mode_MODE_MOUSE_SIMULATION
                             : zmk_touchpad_Mode_MODE_GESTURE;
        // LOG_WRN("  mode=%d", layer_msg.mode);

        layer_msg.bindings_count = TOUCHPAD_NUM_BINDINGS;
        for (int b = 0; b < TOUCHPAD_NUM_BINDINGS; b++) {
            const struct zmk_behavior_binding *binding = zmk_touchpad_get_binding(lidx, b);
            if (binding && binding->behavior_dev) {
                zmk_behavior_local_id_t bid = zmk_behavior_get_local_id(binding->behavior_dev);
                if (bid > 0) {
                    layer_msg.bindings[b].behavior_id = bid;
                    layer_msg.bindings[b].param1 = binding->param1;
                    layer_msg.bindings[b].param2 = binding->param2;
                    // LOG_WRN("  bind[%d]: id=%d p1=%d p2=%d", b, bid, binding->param1,
                    // binding->param2);
                }
            }
        }

        if (!pb_encode_tag_for_field(stream, field)) {
            return false;
        }
        if (!pb_encode_submessage(stream, &zmk_touchpad_Layer_msg, &layer_msg)) {
            return false;
        }
    }
    return true;
}

zmk_studio_Response get_config(const zmk_studio_Request *req) {
    // LOG_WRN("get_config: encoding %d layers", ZMK_KEYMAP_LAYERS_LEN);
    zmk_touchpad_TouchpadConfig config = zmk_touchpad_TouchpadConfig_init_zero;
    config.layers.funcs.encode = encode_layers;
    config.sensitivity = zmk_touchpad_get_sensitivity();
    return TP_RESPONSE(get_config, config);
}

zmk_studio_Response set_mode(const zmk_studio_Request *req) {
    // LOG_DBG("");
    const zmk_touchpad_SetTouchpadModeRequest *set_req =
        &req->subsystem.touchpad.request_type.set_mode;

    zmk_keymap_layer_index_t layer_idx = zmk_keymap_layer_id_to_index(set_req->layer_id);
    if (layer_idx == ZMK_KEYMAP_LAYER_ID_INVAL) {
        zmk_touchpad_SetTouchpadModeResponse resp = zmk_touchpad_SetTouchpadModeResponse_init_zero;
        resp.result = zmk_touchpad_ModeResult_MODE_INVALID_LAYER;
        return TP_RESPONSE(set_mode, resp);
    }

    touchpad_mode_t mode = (set_req->mode == zmk_touchpad_Mode_MODE_MOUSE_SIMULATION)
                               ? TOUCHPAD_MODE_MOUSE_SIMULATION
                               : TOUCHPAD_MODE_GESTURE;

    zmk_touchpad_SetTouchpadModeResponse resp = zmk_touchpad_SetTouchpadModeResponse_init_zero;

    int ret = zmk_touchpad_set_mode(layer_idx, mode);
    if (ret < 0) {
        resp.result = zmk_touchpad_ModeResult_MODE_INVALID_LAYER;
        return TP_RESPONSE(set_mode, resp);
    }

    resp.result = zmk_touchpad_ModeResult_MODE_OK;
    {
        zmk_studio_Notification notify = KEYMAP_NOTIFICATION(unsaved_changes_status_changed, true);
        raise_zmk_studio_rpc_notification(
            (struct zmk_studio_rpc_notification){.notification = notify});
    }
    return TP_RESPONSE(set_mode, resp);
}

zmk_studio_Response set_layer_bindings(const zmk_studio_Request *req) {
    // LOG_DBG("");
    const zmk_touchpad_SetLayerBindingsRequest *set_req =
        &req->subsystem.touchpad.request_type.set_layer_bindings;

    zmk_keymap_layer_index_t layer_idx = zmk_keymap_layer_id_to_index(set_req->layer_id);
    if (layer_idx == ZMK_KEYMAP_LAYER_ID_INVAL) {
        zmk_touchpad_SetLayerBindingsResponse resp =
            zmk_touchpad_SetLayerBindingsResponse_init_zero;
        resp.result = zmk_touchpad_BindResult_BIND_INVALID_LAYER;
        return TP_RESPONSE(set_layer_bindings, resp);
    }

    zmk_touchpad_SetLayerBindingsResponse resp = zmk_touchpad_SetLayerBindingsResponse_init_zero;

    for (int b = 0; b < TOUCHPAD_NUM_BINDINGS; b++) {
        struct zmk_behavior_binding binding = {0};

        if (b < set_req->bindings_count) {
            const zmk_keymap_BehaviorBinding *req_binding = &set_req->bindings[b];

            const char *behavior_name =
                zmk_behavior_find_behavior_name_from_local_id(req_binding->behavior_id);

            if (!behavior_name) {
                resp.result = zmk_touchpad_BindResult_BIND_INVALID_BEHAVIOR;
                return TP_RESPONSE(set_layer_bindings, resp);
            }

            binding.behavior_dev = behavior_name;
            binding.param1 = req_binding->param1;
            binding.param2 = req_binding->param2;
        }

        int ret = zmk_touchpad_set_binding(layer_idx, b, binding);
        if (ret < 0) {
            resp.result = zmk_touchpad_BindResult_BIND_INVALID_LAYER;
            return TP_RESPONSE(set_layer_bindings, resp);
        }
    }

    resp.result = zmk_touchpad_BindResult_BIND_OK;
    {
        zmk_studio_Notification notify = KEYMAP_NOTIFICATION(unsaved_changes_status_changed, true);
        raise_zmk_studio_rpc_notification(
            (struct zmk_studio_rpc_notification){.notification = notify});
    }
    return TP_RESPONSE(set_layer_bindings, resp);
}

zmk_studio_Response set_sensitivity(const zmk_studio_Request *req) {
    const zmk_touchpad_SetSensitivityRequest *set_req =
        &req->subsystem.touchpad.request_type.set_sensitivity;

    zmk_touchpad_SetSensitivityResponse resp = zmk_touchpad_SetSensitivityResponse_init_zero;

    if (set_req->sensitivity < 100 || set_req->sensitivity > 500) {
        resp.result = zmk_touchpad_SensitivityResult_SENSITIVITY_INVALID_VALUE;
        return TP_RESPONSE(set_sensitivity, resp);
    }

    int ret = zmk_touchpad_set_sensitivity(set_req->sensitivity);
    if (ret < 0) {
        resp.result = zmk_touchpad_SensitivityResult_SENSITIVITY_INVALID_VALUE;
        return TP_RESPONSE(set_sensitivity, resp);
    }

    resp.result = zmk_touchpad_SensitivityResult_SENSITIVITY_OK;
    {
        zmk_studio_Notification notify = KEYMAP_NOTIFICATION(unsaved_changes_status_changed, true);
        raise_zmk_studio_rpc_notification(
            (struct zmk_studio_rpc_notification){.notification = notify});
    }
    return TP_RESPONSE(set_sensitivity, resp);
}

static zmk_studio_Response check_unsaved_changes(const zmk_studio_Request *req) {
    // LOG_DBG("");
    return TP_RESPONSE(check_unsaved_changes, zmk_touchpad_check_unsaved_changes() > 0);
}

static zmk_studio_Response save_changes(const zmk_studio_Request *req) {
    zmk_touchpad_SaveChangesResponse resp = zmk_touchpad_SaveChangesResponse_init_zero;
    resp.which_result = zmk_touchpad_SaveChangesResponse_ok_tag;
    resp.result.ok = true;

    int ret = zmk_touchpad_save_changes();
    if (ret < 0) {
        resp.which_result = zmk_touchpad_SaveChangesResponse_err_tag;
        resp.result.err = zmk_touchpad_SaveResult_SAVE_ERR;
    } else {
        zmk_studio_Notification notify = TP_NOTIFICATION(unsaved_changes_status_changed, false);
        raise_zmk_studio_rpc_notification(
            (struct zmk_studio_rpc_notification){.notification = notify});
    }

    return TP_RESPONSE(save_changes, resp);
}

static zmk_studio_Response discard_changes(const zmk_studio_Request *req) {
    // LOG_DBG("");
    int ret = zmk_touchpad_discard_changes();
    if (ret < 0) {
        return ZMK_RPC_SIMPLE_ERR(GENERIC);
    }

    {
        zmk_studio_Notification notify = TP_NOTIFICATION(unsaved_changes_status_changed, false);
        raise_zmk_studio_rpc_notification(
            (struct zmk_studio_rpc_notification){.notification = notify});
    }

    return TP_RESPONSE(discard_changes, true);
}

static int touchpad_settings_reset(void) { return zmk_touchpad_reset_settings(); }

ZMK_RPC_SUBSYSTEM_SETTINGS_RESET(touchpad, touchpad_settings_reset);

ZMK_RPC_SUBSYSTEM_HANDLER(touchpad, get_config, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(touchpad, set_mode, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(touchpad, set_layer_bindings, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(touchpad, check_unsaved_changes, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(touchpad, save_changes, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(touchpad, discard_changes, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(touchpad, set_sensitivity, ZMK_STUDIO_RPC_HANDLER_SECURED);
