/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Dev/bench shell for the Coban Tab 1a key matrix (backlog a13, replaces
 * the retired `vkeymap` model shell). The tab's keys are real keymap
 * positions (see zmk-config cobantab1a.keymap), so this shell speaks
 * straight to the same keymap APIs Studio's RPC uses — bindings it writes
 * are the bindings Studio sees. Until a14 wires the Studio transport this
 * is how the bench rebinds tiles and renames macros without a rebuild;
 * the keymap screen redraws live via its layer/macro listeners.
 *
 * Staging follows Studio: bind/clear/macro-name mark unsaved changes,
 * `vkey save` / `vkey macro-save` persist them to settings.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>

#include <zmk/behavior.h>
#include <zmk/behavior_dynamic_macro.h>
#include <zmk/keymap.h>
#include <zmk/matrix.h>

LOG_MODULE_REGISTER(zmk_vkey, CONFIG_ZMK_LOG_LEVEL);

static int parse_u32(const char *s, uint32_t *out) {
    char *end;
    unsigned long v = strtoul(s, &end, 0);
    if (end == s || *end != '\0' || v > UINT32_MAX) {
        return -EINVAL;
    }
    *out = (uint32_t)v;
    return 0;
}

static int layer_of(const struct shell *sh, const char *arg, zmk_keymap_layer_id_t *out) {
    uint32_t idx;
    if (parse_u32(arg, &idx) || idx >= ZMK_KEYMAP_LAYERS_LEN) {
        shell_error(sh, "bad layer index (0..%u)", ZMK_KEYMAP_LAYERS_LEN - 1);
        return -EINVAL;
    }
    zmk_keymap_layer_id_t id = zmk_keymap_layer_index_to_id((zmk_keymap_layer_index_t)idx);
    if (id == ZMK_KEYMAP_LAYER_ID_INVAL) {
        shell_error(sh, "layer %u does not exist", idx);
        return -EINVAL;
    }
    *out = id;
    return 0;
}

static int cmd_show(const struct shell *sh, size_t argc, char **argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    for (zmk_keymap_layer_index_t idx = 0;; idx++) {
        zmk_keymap_layer_id_t id = zmk_keymap_layer_index_to_id(idx);
        if (id == ZMK_KEYMAP_LAYER_ID_INVAL) {
            break;
        }

        const char *name = zmk_keymap_layer_name(id);
        shell_print(sh, "layer %u id %u '%s'%s:", idx, id, name ? name : "?",
                    zmk_keymap_layer_active(id) ? " (active)" : "");

        for (uint32_t pos = 0; pos < ZMK_KEYMAP_LEN; pos++) {
            const struct zmk_behavior_binding *b = zmk_keymap_get_layer_binding_at_idx(id, pos);
            if (b == NULL || b->behavior_dev == NULL) {
                continue;
            }
            if (strcmp(b->behavior_dev, "none") == 0) {
                continue;
            }
            shell_print(sh, "  [%2u] %-20s p=%u,%u", pos, b->behavior_dev, b->param1, b->param2);
        }
    }
    return 0;
}

/* vkey bind <layer-idx> <pos> <behavior> <param1> <param2> */
static int cmd_bind(const struct shell *sh, size_t argc, char **argv) {
    if (argc != 6) {
        shell_error(sh, "usage: vkey bind <layer> <pos> <behavior> <p1> <p2>");
        return -EINVAL;
    }

    zmk_keymap_layer_id_t layer;
    uint32_t pos, p1, p2;
    if (layer_of(sh, argv[1], &layer) || parse_u32(argv[2], &pos) || parse_u32(argv[4], &p1) ||
        parse_u32(argv[5], &p2)) {
        shell_error(sh, "bad argument");
        return -EINVAL;
    }
    if (pos >= ZMK_KEYMAP_LEN) {
        shell_error(sh, "pos out of range (0..%u)", ZMK_KEYMAP_LEN - 1);
        return -EINVAL;
    }
    if (zmk_behavior_get_binding(argv[3]) == NULL) {
        shell_error(sh, "unknown behavior device '%s'", argv[3]);
        return -EINVAL;
    }

    struct zmk_behavior_binding binding = {
        .behavior_dev = argv[3],
        .param1 = p1,
        .param2 = p2,
    };

    int err = zmk_keymap_set_layer_binding_at_idx(layer, pos, binding);
    if (err) {
        shell_error(sh, "bind failed (err %d)", err);
        return err;
    }
    shell_print(sh, "bound pos %u on layer id %u -> %s %u,%u (vkey save to persist)", pos, layer,
                argv[3], p1, p2);
    return 0;
}

/* vkey clear <layer-idx> <pos> */
static int cmd_clear(const struct shell *sh, size_t argc, char **argv) {
    if (argc != 3) {
        shell_error(sh, "usage: vkey clear <layer> <pos>");
        return -EINVAL;
    }

    zmk_keymap_layer_id_t layer;
    uint32_t pos;
    if (layer_of(sh, argv[1], &layer) || parse_u32(argv[2], &pos) || pos >= ZMK_KEYMAP_LEN) {
        shell_error(sh, "bad argument");
        return -EINVAL;
    }

    struct zmk_behavior_binding none = {.behavior_dev = "none", .param1 = 0, .param2 = 0};
    int err = zmk_keymap_set_layer_binding_at_idx(layer, pos, none);
    if (err) {
        shell_error(sh, "clear failed (err %d)", err);
    }
    return err;
}

static int cmd_save(const struct shell *sh, size_t argc, char **argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    int err = zmk_keymap_save_changes();
    if (err) {
        shell_error(sh, "keymap save failed (err %d)", err);
    }
    return err;
}

static int cmd_discard(const struct shell *sh, size_t argc, char **argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    int err = zmk_keymap_discard_changes();
    if (err) {
        shell_error(sh, "keymap discard failed (err %d)", err);
    }
    return err;
}

/* vkey macro-name <macro-idx> <NAME> */
static int cmd_macro_name(const struct shell *sh, size_t argc, char **argv) {
    if (argc != 4) {
        shell_error(sh, "usage: vkey macro-name <idx> <NAME>");
        return -EINVAL;
    }

    uint32_t idx;
    if (parse_u32(argv[1], &idx) || idx >= (uint32_t)zmk_dynamic_macro_get_count()) {
        shell_error(sh, "macro idx out of range (0..%d)", zmk_dynamic_macro_get_count() - 1);
        return -EINVAL;
    }

    int err = zmk_dynamic_macro_set_name(idx, argv[2]);
    if (err) {
        shell_error(sh, "set name failed (err %d; max %d chars)", err,
                    ZMK_DYNAMIC_MACRO_NAME_MAX - 1);
        return err;
    }
    shell_print(sh, "macro %u named \"%s\" (vkey macro-save to persist)", idx, argv[2]);
    return 0;
}

static int cmd_macro_save(const struct shell *sh, size_t argc, char **argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    int err = zmk_dynamic_macro_save_changes();
    if (err) {
        shell_error(sh, "macro save failed (err %d)", err);
    }
    return err;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
    vkey_cmds,
    SHELL_CMD(show, NULL, "List keymap layers, positions and bindings", cmd_show),
    SHELL_CMD(bind, NULL, "Bind a position: <layer> <pos> <behavior> <p1> <p2>", cmd_bind),
    SHELL_CMD(clear, NULL, "Bind &none: <layer> <pos>", cmd_clear),
    SHELL_CMD(save, NULL, "Persist staged keymap changes", cmd_save),
    SHELL_CMD(discard, NULL, "Revert staged keymap changes", cmd_discard),
    SHELL_CMD(macro_name, NULL, "Rename a dynamic macro: <idx> <NAME>", cmd_macro_name),
    SHELL_CMD(macro_save, NULL, "Persist staged macro changes", cmd_macro_save),
    SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(vkey, &vkey_cmds, "Tab 1a key matrix / dynamic macro dev shell", NULL);
