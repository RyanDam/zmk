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

#if IS_ENABLED(CONFIG_BT)
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#endif

#include <zmk/behavior.h>
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO)
#include <zmk/behavior_dynamic_macro.h>
#endif
#include <zmk/events/position_state_changed.h>
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

/* vkey macro-name <macro-idx> <NAME> — Studio-subsystem dependent, so it
 * disappears in builds without it (e.g. the a13 BLE bisection variants). */
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO)
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
#endif /* CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO */

/* ------------------------------------------------------------------ */
/* Bench edges: vkey press / release / tap                             */
/* ------------------------------------------------------------------ */

/*
 * Raise real position edges with no keys and no display, to exercise the
 * full keymap -> HID -> BLE path (a13 BLE-crash bisection: the prototype
 * has no wired buttons, and the display must be switchable off). Edges
 * go through a work item on the system work queue — the same context
 * kscan and the keymap screen's edge work raise from — and the held
 * guard refuses unpaired edges, mirroring the kscan contract (a13 owner
 * repro: unpaired edges from LVGL stuck the keymap; this tool must not
 * fabricate that by accident, only to order it deliberately).
 */

static struct k_work fire_press_work;
static struct zmk_position_state_changed fire_press_ev;

static struct k_work_delayable fire_release_dw;
static struct zmk_position_state_changed fire_release_ev;

static bool fire_held;
static uint32_t fire_held_pos;

static struct zmk_position_state_changed fire_edge(uint32_t pos, bool pressed) {
    return (struct zmk_position_state_changed){
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
        .position = pos,
        .state = pressed,
        .timestamp = k_uptime_get(),
    };
}

static void fire_raise(struct zmk_position_state_changed *ev) {
    int err = raise_zmk_position_state_changed(*ev);

    if (err) {
        LOG_WRN("vkey fire raise failed (err %d)", err);
    }
}

static void fire_press_cb(struct k_work *work) {
    ARG_UNUSED(work);
    fire_raise(&fire_press_ev);
}

static void fire_release_cb(struct k_work *work) {
    ARG_UNUSED(work);
    if (!fire_held) {
        return; /* manual release already got there first */
    }
    fire_held = false;
    fire_raise(&fire_release_ev);
}

static int fire_parse(const struct shell *sh, const char *arg, uint32_t *pos) {
    if (parse_u32(arg, pos) || *pos >= ZMK_KEYMAP_LEN) {
        shell_error(sh, "pos out of range (0..%u)", ZMK_KEYMAP_LEN - 1);
        return -EINVAL;
    }
    return 0;
}

/* vkey press <pos> — hold a position down */
static int cmd_press(const struct shell *sh, size_t argc, char **argv) {
    if (argc != 2) {
        shell_error(sh, "usage: vkey press <pos>");
        return -EINVAL;
    }
    uint32_t pos;
    if (fire_parse(sh, argv[1], &pos)) {
        return -EINVAL;
    }
    if (fire_held) {
        shell_error(sh, "pos %u is still held (release or tap first)", fire_held_pos);
        return -EBUSY;
    }

    fire_press_ev = fire_edge(pos, true);
    fire_held = true;
    fire_held_pos = pos;
    k_work_submit(&fire_press_work);
    shell_print(sh, "pos %u pressed", pos);
    return 0;
}

/* vkey release — raise the held position, canceling any pending tap */
static int cmd_release(const struct shell *sh, size_t argc, char **argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    if (!fire_held) {
        shell_error(sh, "nothing held");
        return -EINVAL;
    }

    struct k_work_sync sync;
    k_work_cancel_delayable_sync(&fire_release_dw, &sync);
    if (fire_held) { /* auto-release had not fired: do it ourselves */
        fire_held = false;
        fire_release_ev = fire_edge(fire_held_pos, false);
        fire_raise(&fire_release_ev);
    }
    shell_print(sh, "pos %u released", fire_held_pos);
    return 0;
}

/* vkey tap <pos> [hold-ms, default 100] — press, auto-release after hold */
static int cmd_tap(const struct shell *sh, size_t argc, char **argv) {
    if (argc < 2 || argc > 3) {
        shell_error(sh, "usage: vkey tap <pos> [hold-ms]");
        return -EINVAL;
    }
    uint32_t pos, hold_ms = 100;
    if (fire_parse(sh, argv[1], &pos) || (argc == 3 && parse_u32(argv[2], &hold_ms))) {
        shell_error(sh, "bad argument");
        return -EINVAL;
    }
    if (fire_held) {
        shell_error(sh, "pos %u is still held (release first)", fire_held_pos);
        return -EBUSY;
    }

    fire_press_ev = fire_edge(pos, true);
    fire_release_ev = fire_edge(pos, false);
    fire_held = true;
    fire_held_pos = pos;
    k_work_submit(&fire_press_work);
    k_work_schedule(&fire_release_dw, K_MSEC(hold_ms));
    shell_print(sh, "pos %u tapped (%u ms hold)", pos, hold_ms);
    return 0;
}

static int fire_init(void) {
    k_work_init(&fire_press_work, fire_press_cb);
    k_work_init_delayable(&fire_release_dw, fire_release_cb);
    return 0;
}
SYS_INIT(fire_init, APPLICATION, 90);

/* ------------------------------------------------------------------ */
/* Bench link: vkey link [interval] [latency]                          */
/* ------------------------------------------------------------------ */

/*
 * a13 BLE-crash bisection probe: the Mac always negotiates interval 15 ms
 * with slave latency 22, and every wedge lands inside BLE traffic with no
 * panic ever printed - the ESP32-S3 controller hanging on slave-latency
 * connection events is a known failure family. This forces our own
 * connection-parameter update (latency 0 by default) on the live link; if
 * the device then survives the hammer, the wedge is latency-driven and
 * the product fix is to always request latency 0.
 */
static struct bt_conn *bench_conn;

static void bench_connected(struct bt_conn *conn, uint8_t err) {
    if (!err && bench_conn == NULL) {
        bench_conn = bt_conn_ref(conn);
    }
}

static void bench_disconnected(struct bt_conn *conn, uint8_t reason) {
    ARG_UNUSED(reason);
    if (bench_conn == conn) {
        bench_conn = NULL;
        bt_conn_unref(conn);
    }
}

BT_CONN_CB_DEFINE(bench_conn_cb) = {
    .connected = bench_connected,
    .disconnected = bench_disconnected,
};

static int cmd_link(const struct shell *sh, size_t argc, char **argv) {
#if IS_ENABLED(CONFIG_BT)
    uint32_t interval = 24, latency = 0; /* 30 ms, no slave latency */
    if (argc >= 2 && parse_u32(argv[1], &interval)) {
        shell_error(sh, "bad interval (units of 1.25 ms)");
        return -EINVAL;
    }
    if (argc >= 3 && parse_u32(argv[2], &latency)) {
        shell_error(sh, "bad latency");
        return -EINVAL;
    }

    struct bt_conn *conn = bench_conn;
    if (conn == NULL) {
        shell_error(sh, "no connected peer (tracked via conn cb)");
        return -ENOTCONN;
    }

    struct bt_le_conn_param param = {
        .interval_min = (uint16_t)interval,
        .interval_max = (uint16_t)interval,
        .latency = (uint16_t)latency,
        .timeout = 400,
    };
    int err = bt_conn_le_param_update(conn, &param);
    if (err) {
        shell_error(sh, "bt_conn_update failed (%d)", err);
        return err;
    }
    shell_print(sh, "conn update requested: interval %u latency %u", interval, latency);
    return 0;
#else
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    shell_error(sh, "BT disabled in this build");
    return -ENOTSUP;
#endif
}

SHELL_STATIC_SUBCMD_SET_CREATE(
    vkey_cmds,
    SHELL_CMD(show, NULL, "List keymap layers, positions and bindings", cmd_show),
    SHELL_CMD(bind, NULL, "Bind a position: <layer> <pos> <behavior> <p1> <p2>", cmd_bind),
    SHELL_CMD(clear, NULL, "Bind &none: <layer> <pos>", cmd_clear),
    SHELL_CMD(save, NULL, "Persist staged keymap changes", cmd_save),
    SHELL_CMD(discard, NULL, "Revert staged keymap changes", cmd_discard),
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO)
    SHELL_CMD(macro_name, NULL, "Rename a dynamic macro: <idx> <NAME>", cmd_macro_name),
    SHELL_CMD(macro_save, NULL, "Persist staged macro changes", cmd_macro_save),
#endif
    SHELL_CMD(press, NULL, "Bench: hold a keymap position down: <pos>", cmd_press),
    SHELL_CMD(release, NULL, "Bench: raise the held position up", cmd_release),
    SHELL_CMD(tap, NULL, "Bench: press + auto-release: <pos> [hold-ms]", cmd_tap),
    SHELL_CMD(link, NULL, "Bench: force conn params: [interval] [latency]", cmd_link),
    SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(vkey, &vkey_cmds, "Tab 1a key matrix / dynamic macro dev shell", NULL);
