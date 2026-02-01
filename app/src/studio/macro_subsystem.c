/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk_studio, CONFIG_ZMK_STUDIO_LOG_LEVEL);

#include <zmk/studio/rpc.h>
#include <zmk/behavior_dynamic_macro.h>
#include <pb_encode.h>
#include <pb_decode.h>

#ifdef CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO

ZMK_RPC_SUBSYSTEM(macros)

#define MACROS_Response(type, ...) ZMK_RPC_RESPONSE(macros, type, __VA_ARGS__)
#define MACROS_Notification(type, ...) ZMK_RPC_NOTIFICATION(macros, type, __VA_ARGS__)

#define ZMK_RPC_SUBSYSTEM_HANDLER_EX(prefix, request, func_name, _security)                        \
    STRUCT_SECTION_ITERABLE(zmk_rpc_subsystem_handler,                                             \
                            prefix##_subsystem_handler_##request) = {                              \
        .func = func_name,                                                                         \
        .subsystem_choice = zmk_studio_Request_##prefix##_tag,                                     \
        .request_choice = zmk_##prefix##_Request_##request##_tag,                                  \
        .security = _security,                                                                     \
    };

static bool encode_macro_info(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    int count = zmk_dynamic_macro_get_count();
    int max_steps = zmk_dynamic_macro_get_max_steps();

    for (int i = 0; i < count; i++) {
        zmk_macros_MacroInfo info = zmk_macros_MacroInfo_init_zero;
        info.index = i;
        info.max_steps = max_steps;

        if (!pb_encode_tag_for_field(stream, field)) {
            return false;
        }
        if (!pb_encode_submessage(stream, &zmk_macros_MacroInfo_msg, &info)) {
            return false;
        }
    }
    return true;
}

zmk_studio_Response get_macro_list(const zmk_studio_Request *req) {
    zmk_macros_MacroList list = zmk_macros_MacroList_init_zero;
    list.macros.funcs.encode = encode_macro_info;
    return MACROS_Response(get_macro_list, list);
}

static bool encode_macro_steps(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    uint32_t macro_idx = (uint32_t)((uintptr_t)*arg);
    int max_steps = zmk_dynamic_macro_get_max_steps();

    for (int i = 0; i < max_steps; i++) {
        struct zmk_dynamic_macro_step *step = zmk_dynamic_macro_get_step(macro_idx, i);
        // if (!step || !step->binding.behavior_dev) {
        if (!step || !step->behavior_local_id) {
            break; 
        }

        zmk_macros_MacroStep msg = zmk_macros_MacroStep_init_zero;
        
        switch(step->mode) {
            case MACRO_MODE_TAP: msg.mode = zmk_macros_MacroMode_MACRO_MODE_TAP; break;
            case MACRO_MODE_PRESS: msg.mode = zmk_macros_MacroMode_MACRO_MODE_PRESS; break;
            case MACRO_MODE_RELEASE: msg.mode = zmk_macros_MacroMode_MACRO_MODE_RELEASE; break;
            default: msg.mode = zmk_macros_MacroMode_MACRO_MODE_TAP; break;
        }
        
        msg.wait_ms = step->wait_ms;
        msg.tap_ms = step->tap_ms;
        
        msg.has_binding = true;
        // msg.binding.behavior_id = zmk_behavior_get_local_id(step->binding.behavior_dev);
        msg.binding.behavior_id = step->behavior_local_id;
        msg.binding.param1 = step->binding.param1;
        msg.binding.param2 = step->binding.param2;

        LOG_DBG("macro %d step %d: binding %d param1 %d param2 %d mode %d wait_ms %d tap_ms %d", 
                macro_idx, i, msg.binding.behavior_id, msg.binding.param1, msg.binding.param2, msg.mode, msg.wait_ms, msg.tap_ms);

        if (!pb_encode_tag_for_field(stream, field)) {
            LOG_DBG("macro %d step %d: fail pb_encode_tag_for_field", macro_idx, i);
            return false;
        }
        if (!pb_encode_submessage(stream, &zmk_macros_MacroStep_msg, &msg)) {
            LOG_DBG("macro %d step %d: fail pb_encode_submessage", macro_idx, i);
            return false;
        }
    }
    return true;
}

zmk_studio_Response get_macro_details(const zmk_studio_Request *req) {
    LOG_DBG("");
    uint32_t macro_idx = req->subsystem.macros.request_type.get_macro_details;
    if (macro_idx >= zmk_dynamic_macro_get_count()) {
        LOG_DBG("macro idx %d invalid", macro_idx);
        return ZMK_RPC_SIMPLE_ERR(GENERIC);
    }

    zmk_macros_MacroDetails details = zmk_macros_MacroDetails_init_zero;
    details.index = macro_idx;
    details.steps.funcs.encode = encode_macro_steps;
    details.steps.arg = (void *)((uintptr_t)macro_idx);

    return MACROS_Response(get_macro_details, details);
}

zmk_studio_Response set_macro_details(const zmk_studio_Request *req) {
    const zmk_macros_SetMacroDetailsRequest *set_req = &req->subsystem.macros.request_type.set_macro_details;
    
    LOG_DBG("idx %d, step count: %d", set_req->index, set_req->steps_count);

    int count = set_req->steps_count;
    int max_steps = zmk_dynamic_macro_get_max_steps();
    
    // Reuse a single step structure to avoid repeated allocations
    struct zmk_dynamic_macro_step step;
    
    for (int i = 0; i < count; i++) {
        const zmk_macros_MacroStep *msg = &set_req->steps[i];
        const zmk_keymap_BehaviorBinding *bb = &msg->binding;
        
        const char *behavior_name = zmk_behavior_find_behavior_name_from_local_id(bb->behavior_id);
        if (!behavior_name) {
             continue;
        }
        
        // Direct assignment instead of struct initialization
        step.binding.behavior_dev = behavior_name;
        step.binding.param1 = bb->param1;
        step.binding.param2 = bb->param2;
        step.behavior_local_id = bb->behavior_id;
        step.wait_ms = msg->wait_ms;
        step.tap_ms = msg->tap_ms;
        
        // Map enum using if-else for better performance than switch
        if (msg->mode == zmk_macros_MacroMode_MACRO_MODE_TAP) {
            step.mode = MACRO_MODE_TAP;
        } else if (msg->mode == zmk_macros_MacroMode_MACRO_MODE_PRESS) {
            step.mode = MACRO_MODE_PRESS;
        } else {
            step.mode = MACRO_MODE_RELEASE;
        }
        
        zmk_dynamic_macro_set_step(set_req->index, i, step);
        LOG_DBG("idx %d, step store: %d, behavior: %s, wait: %d, tap: %d, mode: %d, param1: %d, param2: %d", 
                set_req->index, i, behavior_name, step.wait_ms, step.tap_ms, step.mode, step.binding.param1, step.binding.param2);
    }
    
    // Clear remaining steps
    struct zmk_dynamic_macro_step empty = {0};
    for (int i = count; i < max_steps; i++) {
        zmk_dynamic_macro_set_step(set_req->index, i, empty);
    }

    LOG_DBG("idx %d, DONE", set_req->index);

    raise_zmk_studio_rpc_notification((struct zmk_studio_rpc_notification){
        .notification = MACROS_Notification(unsaved_changes_status_changed, true)});

    return MACROS_Response(set_macro_details, zmk_macros_SetMacroDetailsResponse_SET_MACRO_DETAILS_RESP_OK);
}

zmk_studio_Response macros_check_unsaved_changes(const zmk_studio_Request *req) {
    bool unsaved = zmk_dynamic_macro_check_unsaved_changes();
    return MACROS_Response(check_unsaved_changes, unsaved);
}

zmk_studio_Response macros_save_changes(const zmk_studio_Request *req) {
    zmk_macros_SaveChangesResponse resp = zmk_macros_SaveChangesResponse_init_zero;
    resp.which_result = zmk_macros_SaveChangesResponse_ok_tag;
    resp.result.ok = true;

    int ret = zmk_dynamic_macro_save_changes();
    if (ret < 0) {
        resp.which_result = zmk_macros_SaveChangesResponse_err_tag;
        resp.result.err = zmk_macros_SaveChangesErrorCode_SAVE_CHANGES_ERR_GENERIC;
    } else {
        raise_zmk_studio_rpc_notification((struct zmk_studio_rpc_notification){
            .notification = MACROS_Notification(unsaved_changes_status_changed, false)});
    }

    return MACROS_Response(save_changes, resp);
}

zmk_studio_Response macros_discard_changes(const zmk_studio_Request *req) {
    int ret = zmk_dynamic_macro_discard_changes();
    if (ret < 0) {
        return ZMK_RPC_SIMPLE_ERR(GENERIC);
    }

    raise_zmk_studio_rpc_notification((struct zmk_studio_rpc_notification){
        .notification = MACROS_Notification(unsaved_changes_status_changed, false)});

    return MACROS_Response(discard_changes, true);
}

ZMK_RPC_SUBSYSTEM_HANDLER(macros, get_macro_list, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(macros, get_macro_details, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(macros, set_macro_details, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER_EX(macros, check_unsaved_changes, macros_check_unsaved_changes, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER_EX(macros, save_changes, macros_save_changes, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER_EX(macros, discard_changes, macros_discard_changes, ZMK_STUDIO_RPC_HANDLER_SECURED);

#endif // CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO
