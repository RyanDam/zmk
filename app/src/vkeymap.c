/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Virtual key matrix model implementation (backlog a13). See vkeymap.h.
 *
 * Firing deliberately goes through zmk_behavior_invoke_binding() — the
 * same call keymap.c makes for a hardware keypress — so press-hold
 * behaviors, macro press/release modes and a14's future bindings all
 * behave exactly like physical keys. The release edge lands 30 ms later
 * on the system workqueue (long enough that tap-mode macros read as a
 * real key, short enough to be invisible).
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <zmk/behavior.h>
#include <zmk/behavior_dynamic_macro.h>
#include <zmk/keymap.h>
#include <zmk/vkeymap.h>

LOG_MODULE_REGISTER(zmk_vkeymap, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/settings_blob.h>

#define BLOB_VERSION 1
#define RELEASE_DELAY_MS 30
#define LISTENER_MAX 4

struct layer {
    char name[ZMK_VKEYMAP_NAME_MAX];
    struct zmk_vkeymap_cell cells[ZMK_VKEYMAP_CELLS];
};

static struct layer layers[ZMK_VKEYMAP_LAYERS] = {
    /* layer 0 defaults: the fork's 16 dynamic macros, M1..M16 */
    [0] =
        {
            .name = "Macros",
            .cells =
                {
                    {.name = "M1", .behavior = "dynamic_macro", .param1 = 0},
                    {.name = "M2", .behavior = "dynamic_macro", .param1 = 1},
                    {.name = "M3", .behavior = "dynamic_macro", .param1 = 2},
                    {.name = "M4", .behavior = "dynamic_macro", .param1 = 3},
                    {.name = "M5", .behavior = "dynamic_macro", .param1 = 4},
                    {.name = "M6", .behavior = "dynamic_macro", .param1 = 5},
                    {.name = "M7", .behavior = "dynamic_macro", .param1 = 6},
                    {.name = "M8", .behavior = "dynamic_macro", .param1 = 7},
                    {.name = "M9", .behavior = "dynamic_macro", .param1 = 8},
                    {.name = "M10", .behavior = "dynamic_macro", .param1 = 9},
                    {.name = "M11", .behavior = "dynamic_macro", .param1 = 10},
                    {.name = "M12", .behavior = "dynamic_macro", .param1 = 11},
                    {.name = "M13", .behavior = "dynamic_macro", .param1 = 12},
                    {.name = "M14", .behavior = "dynamic_macro", .param1 = 13},
                    {.name = "M15", .behavior = "dynamic_macro", .param1 = 14},
                    {.name = "M16", .behavior = "dynamic_macro", .param1 = 15},
                },
        },
    [1] = {.name = "Keys"},
    [2] = {.name = "Sys"},
};

static zmk_vkeymap_changed_cb listeners[LISTENER_MAX];

static void notify_listeners(void) {
    for (int i = 0; i < ARRAY_SIZE(listeners); i++) {
        if (listeners[i] != NULL) {
            listeners[i]();
        }
    }
}

int zmk_vkeymap_add_listener(zmk_vkeymap_changed_cb cb) {
    for (int i = 0; i < ARRAY_SIZE(listeners); i++) {
        if (listeners[i] == cb) {
            return 0;
        }
        if (listeners[i] == NULL) {
            listeners[i] = cb;
            return 0;
        }
    }
    return -ENOSPC;
}

int zmk_vkeymap_layer_count(void) { return ZMK_VKEYMAP_LAYERS; }

const char *zmk_vkeymap_layer_name(unsigned int layer) {
    if (layer >= ZMK_VKEYMAP_LAYERS) {
        return "?";
    }
    return layers[layer].name;
}

const struct zmk_vkeymap_cell *zmk_vkeymap_cell_get(unsigned int layer, unsigned int idx) {
    if (layer >= ZMK_VKEYMAP_LAYERS || idx >= ZMK_VKEYMAP_CELLS) {
        return NULL;
    }
    return &layers[layer].cells[idx];
}

static int valid_range(unsigned int layer, unsigned int idx) {
    return layer < ZMK_VKEYMAP_LAYERS && idx < ZMK_VKEYMAP_CELLS ? 0 : -EINVAL;
}

static bool cell_known_behavior(const struct zmk_vkeymap_cell *cell) {
    if (cell->behavior[0] == '\0') {
        return true; /* clearing is always fine */
    }
    return zmk_behavior_get_binding(cell->behavior) != NULL;
}

/* Debounced save: a burst of edits (shell, later the a14 RPC) hits NVS
 * once. The blob is static, not on the shell/display stack (~1.7 KB).
 * The write lags the live model by one debounce window; at worst a
 * superseded value is what gets persisted. */
#define SAVE_DEBOUNCE_MS 250

struct vkeymap_blob {
    uint32_t version;
    struct layer layers[ZMK_VKEYMAP_LAYERS];
};

static struct vkeymap_blob pending_blob;

static void save_work_fn(struct k_work *work) {
    ARG_UNUSED(work);
    zmk_settings_blob_save("vkeymap/config", &pending_blob, sizeof(pending_blob), "vkeymap");
}
K_WORK_DELAYABLE_DEFINE(save_work, save_work_fn);

static int persist(void) {
    pending_blob.version = BLOB_VERSION;
    memcpy(pending_blob.layers, layers, sizeof(pending_blob.layers));
    k_work_reschedule(&save_work, K_MSEC(SAVE_DEBOUNCE_MS));
    return 0;
}

int zmk_vkeymap_cell_set(unsigned int layer, unsigned int idx,
                         const struct zmk_vkeymap_cell *cell) {
    int err = valid_range(layer, idx);

    if (err || cell == NULL) {
        return err ? err : -EINVAL;
    }
    if (strnlen(cell->name, sizeof(cell->name)) >= sizeof(cell->name) ||
        strnlen(cell->behavior, sizeof(cell->behavior)) >= sizeof(cell->behavior)) {
        return -EMSGSIZE;
    }
    if (!cell_known_behavior(cell)) {
        LOG_ERR("unknown behavior device '%s'", cell->behavior);
        return -EINVAL;
    }

    struct zmk_vkeymap_cell *dst = &layers[layer].cells[idx];
    struct zmk_vkeymap_cell old = *dst;

    *dst = *cell;
    if (memcmp(&old, dst, sizeof(old)) == 0) {
        return 0;
    }

    LOG_INF("vkeymap L%u C%u -> %s '%s' p=%u,%u", layer, idx, dst->name,
            dst->behavior[0] ? dst->behavior : "(dead)", dst->param1, dst->param2);
    persist();
    notify_listeners();
    return 0;
}

int zmk_vkeymap_cell_clear(unsigned int layer, unsigned int idx) {
    struct zmk_vkeymap_cell dead = {0};

    return zmk_vkeymap_cell_set(layer, idx, &dead);
}

/* ------------------------------------------------------------------ */
/* Firing                                                              */
/* ------------------------------------------------------------------ */

/* Ring of presses awaiting their release edge. A single snapshot would
 * let a second tap within the release delay rebind the pending release
 * (press A would release as press B, and press/release-mode behaviors
 * could stick) — so each press keeps its own record. */
#define FIRE_RING 4

struct fire_rec {
    struct zmk_vkeymap_cell cell; /* owns the behavior name for the release */
    struct zmk_behavior_binding_event event;
};

static struct fire_rec fire_ring[FIRE_RING];
static uint8_t fire_in, fire_out;

static void release_one(struct fire_rec *rec) {
    struct zmk_behavior_binding binding = {
        .behavior_dev = rec->cell.behavior,
        .param1 = rec->cell.param1,
        .param2 = rec->cell.param2,
    };

    int err = zmk_behavior_invoke_binding(&binding, rec->event, false);

    if (err) {
        LOG_WRN("vkeymap release edge failed (err %d)", err);
    }
}

static void release_work_fn(struct k_work *work) {
    ARG_UNUSED(work);
    while (fire_out != fire_in) {
        release_one(&fire_ring[fire_out]);
        fire_out = (fire_out + 1) % FIRE_RING;
    }
}

K_WORK_DELAYABLE_DEFINE(release_work, release_work_fn);

int zmk_vkeymap_fire(unsigned int layer, unsigned int idx) {
    const struct zmk_vkeymap_cell *cell = zmk_vkeymap_cell_get(layer, idx);

    if (cell == NULL) {
        return -EINVAL;
    }
    if (cell->behavior[0] == '\0') {
        LOG_DBG("vkeymap L%u C%u unassigned", layer, idx);
        return -EINVAL;
    }

    /* Snapshot: a settings reload could swap the model between edges. */
    if ((fire_in + 1) % FIRE_RING == fire_out) {
        /* ring full: the oldest press cannot wait any longer */
        release_one(&fire_ring[fire_out]);
        fire_out = (fire_out + 1) % FIRE_RING;
    }

    struct fire_rec *rec = &fire_ring[fire_in];

    rec->cell = *cell;
    rec->event = (struct zmk_behavior_binding_event){
        .layer = zmk_keymap_layer_index_to_id(zmk_keymap_highest_layer_active()),
        .position = idx,
        .timestamp = k_uptime_get(),
    };

    struct zmk_behavior_binding binding = {
        .behavior_dev = rec->cell.behavior,
        .param1 = rec->cell.param1,
        .param2 = rec->cell.param2,
    };

    int err = zmk_behavior_invoke_binding(&binding, rec->event, true);

    if (err) {
        LOG_WRN("vkeymap L%u C%u '%s' press failed (err %d)", layer, idx, rec->cell.name, err);
        return err;
    }

    LOG_INF("vkeymap fired L%u C%u '%s' -> %s %u,%u", layer, idx, rec->cell.name,
            rec->cell.behavior, rec->cell.param1, rec->cell.param2);
    fire_in = (fire_in + 1) % FIRE_RING;
    k_work_reschedule(&release_work, K_MSEC(RELEASE_DELAY_MS));
    return 0;
}

/* ------------------------------------------------------------------ */
/* Settings                                                            */
/* ------------------------------------------------------------------ */

#if IS_ENABLED(CONFIG_SETTINGS)
static int vkeymap_settings_handle_set(const char *key, size_t len, settings_read_cb read_cb,
                                       void *cb_arg) {
    struct vkeymap_blob blob;
    int err =
        zmk_settings_blob_read(key, len, read_cb, cb_arg, "config", &blob, sizeof(blob), "vkeymap");

    if (err <= 0) {
        return err;
    }
    if (blob.version != BLOB_VERSION) {
        LOG_WRN("vkeymap settings version %u unsupported - keeping defaults", blob.version);
        return -EINVAL;
    }
    for (unsigned l = 0; l < ZMK_VKEYMAP_LAYERS; l++) {
        for (unsigned c = 0; c < ZMK_VKEYMAP_CELLS; c++) {
            struct zmk_vkeymap_cell *cell = &blob.layers[l].cells[c];

            /* settings are untrusted: reject unknown behaviors, keep the cell */
            if (cell->behavior[0] != '\0' && zmk_behavior_get_binding(cell->behavior) == NULL) {
                LOG_WRN("vkeymap L%u C%u behavior '%s' gone - cell dead", l, c, cell->behavior);
                cell->behavior[0] = '\0';
            }
        }
    }

    memcpy(layers, blob.layers, sizeof(layers));
    LOG_INF("vkeymap model loaded from settings");
    notify_listeners();
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(vkeymap, "vkeymap", NULL, vkeymap_settings_handle_set, NULL, NULL);
#endif /* CONFIG_SETTINGS */

/* ------------------------------------------------------------------ */
/* Dev shell (a13 acceptance: edit names/bindings without a rebuild)   */
/* ------------------------------------------------------------------ */

#if IS_ENABLED(CONFIG_ZMK_VKEYMAP_SHELL)

#include <zephyr/shell/shell.h>

static int cmd_show(const struct shell *sh, size_t argc, char **argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    for (unsigned l = 0; l < ZMK_VKEYMAP_LAYERS; l++) {
        shell_print(sh, "layer %u '%s':", l, layers[l].name);
        for (unsigned c = 0; c < ZMK_VKEYMAP_CELLS; c++) {
            const struct zmk_vkeymap_cell *cell = &layers[l].cells[c];

            if (cell->behavior[0] == '\0' && cell->name[0] == '\0') {
                continue;
            }
            shell_print(sh, "  [%u] %-8s %s %u %u", c, cell->name,
                        cell->behavior[0] ? cell->behavior : "(dead)", cell->param1, cell->param2);
        }
    }
    return 0;
}

/* vkeymap macro <layer> <cell> <macro-idx> [NAME] */
static int cmd_macro(const struct shell *sh, size_t argc, char **argv) {
    const struct zmk_vkeymap_cell *model;
    struct zmk_vkeymap_cell cell;
    long layer, idx, macro;
    int err;

    if (argc < 4 || argc > 5) {
        shell_error(sh, "usage: vkeymap macro <layer> <cell> <macro-idx> [NAME]");
        return -EINVAL;
    }
    layer = strtol(argv[1], NULL, 10);
    idx = strtol(argv[2], NULL, 10);
    macro = strtol(argv[3], NULL, 10);
    if (layer < 0 || layer >= ZMK_VKEYMAP_LAYERS || idx < 0 || idx >= ZMK_VKEYMAP_CELLS ||
        macro < 0 || macro >= zmk_dynamic_macro_get_count()) {
        shell_error(sh, "out of range (layers %u, cells %u, macros %d)", ZMK_VKEYMAP_LAYERS,
                    ZMK_VKEYMAP_CELLS, zmk_dynamic_macro_get_count());
        return -EINVAL;
    }

    model = zmk_vkeymap_cell_get(layer, idx);
    cell = *model;
    cell.param1 = (uint32_t)macro;
    strcpy(cell.behavior, "dynamic_macro");
    if (argc == 5) {
        strncpy(cell.name, argv[4], sizeof(cell.name) - 1);
        cell.name[sizeof(cell.name) - 1] = '\0';
    } else if (cell.name[0] == '\0') {
        snprintk(cell.name, sizeof(cell.name), "M%u", (unsigned int)(macro % 99) + 1U);
    }

    err = zmk_vkeymap_cell_set(layer, idx, &cell);
    if (err) {
        shell_error(sh, "set failed (err %d)", err);
    }
    return err;
}

/* vkeymap bind <layer> <cell> <behavior> <param1> <param2> [NAME] */
static int cmd_bind(const struct shell *sh, size_t argc, char **argv) {
    const struct zmk_vkeymap_cell *model;
    struct zmk_vkeymap_cell cell;
    long layer, idx, p1, p2;
    int err;

    if (argc < 6 || argc > 7) {
        shell_error(sh, "usage: vkeymap bind <layer> <cell> <behavior> <p1> <p2> [NAME]");
        return -EINVAL;
    }
    layer = strtol(argv[1], NULL, 10);
    idx = strtol(argv[2], NULL, 10);
    p1 = strtol(argv[4], NULL, 0);
    p2 = strtol(argv[5], NULL, 0);
    if (layer < 0 || layer >= ZMK_VKEYMAP_LAYERS || idx < 0 || idx >= ZMK_VKEYMAP_CELLS) {
        shell_error(sh, "out of range (layers %u, cells %u)", ZMK_VKEYMAP_LAYERS,
                    ZMK_VKEYMAP_CELLS);
        return -EINVAL;
    }

    model = zmk_vkeymap_cell_get(layer, idx);
    cell = *model;
    cell.param1 = (uint32_t)p1;
    cell.param2 = (uint32_t)p2;
    strncpy(cell.behavior, argv[3], sizeof(cell.behavior) - 1);
    cell.behavior[sizeof(cell.behavior) - 1] = '\0';
    if (argc == 7) {
        strncpy(cell.name, argv[6], sizeof(cell.name) - 1);
        cell.name[sizeof(cell.name) - 1] = '\0';
    }

    err = zmk_vkeymap_cell_set(layer, idx, &cell);
    if (err) {
        shell_error(sh, "set failed (err %d)", err);
    }
    return err;
}

static int cmd_clear(const struct shell *sh, size_t argc, char **argv) {
    long layer, idx;
    int err;

    if (argc != 3) {
        shell_error(sh, "usage: vkeymap clear <layer> <cell>");
        return -EINVAL;
    }
    layer = strtol(argv[1], NULL, 10);
    idx = strtol(argv[2], NULL, 10);
    err = zmk_vkeymap_cell_clear(layer, idx);
    if (err) {
        shell_error(sh, "clear failed (err %d)", err);
    }
    return err;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
    vkeymap_cmds, SHELL_CMD(show, NULL, "List layers and assigned cells", cmd_show),
    SHELL_CMD(macro, NULL, "Assign a dynamic macro: <layer> <cell> <idx> [NAME]", cmd_macro),
    SHELL_CMD(bind, NULL, "Assign a behavior: <layer> <cell> <dev> <p1> <p2> [NAME]", cmd_bind),
    SHELL_CMD(clear, NULL, "Clear a cell: <layer> <cell>", cmd_clear), SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(vkeymap, &vkeymap_cmds, "Tab 1a virtual key matrix model", NULL);

#endif /* CONFIG_ZMK_VKEYMAP_SHELL */
