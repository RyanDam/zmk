/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Asset store v1 implementation (backlog a12). See zmk/assets.h.
 *
 * The partition layout (offset/size) comes from devicetree, so a move
 * of the `assets` partition in the board DTS needs no code change —
 * same discipline as the c02 settings-partition-by-label fix.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zmk/assets.h>

LOG_MODULE_REGISTER(zmk_assets, CONFIG_ZMK_LOG_LEVEL);

#define ASSETS_NODE DT_NODELABEL(assets_partition)

/* flash0 is the unbound soc-nv-flash pseudo-node on this port; the
 * driver is bound on the ESP32 flash controller (chosen
 * zephyr,flash-controller), and partition regs are absolute flash
 * addresses on it. */
static const struct device *const flash_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_flash_controller));
static const uint32_t assets_base = DT_REG_ADDR(ASSETS_NODE);
static const uint32_t assets_size = DT_REG_SIZE(ASSETS_NODE);

/* Packed image format, shared with scripts/display-assets/pack_assets.py
 * — keep the two in sync (magic carries the version). */
#define HEADER_MAGIC "ZMKAS001"
#define HEADER_SIZE 16
#define ENTRY_SIZE 40
#define ENTRY_MAX 32

struct __packed file_header {
    uint8_t magic[8];
    uint32_t entry_count;
    uint32_t flags;
};

struct __packed file_entry {
    char name[ZMK_ASSET_NAME_MAX];
    uint32_t offset;
    uint32_t len;
    uint16_t kind;
    uint16_t fps;
    uint32_t reserved;
};

static struct zmk_asset_info table[ENTRY_MAX];
static int table_count = -1; /* -1 until probed */
static bool store_ready;

static int read_at(uint32_t off, void *buf, uint32_t len) {
    if (off > assets_size || len > assets_size - off) {
        return -EINVAL;
    }
    return flash_read(flash_dev, assets_base + off, buf, len);
}

int zmk_asset_read(const struct zmk_asset_info *asset, uint32_t off, void *buf, uint32_t len) {
    if (asset == NULL || buf == NULL || table_count < 0) {
        return -EINVAL;
    }
    if (off > asset->len || len > asset->len - off) {
        return -EINVAL;
    }
    return read_at(asset->offset + off, buf, len);
}

bool zmk_asset_ready(void) { return store_ready; }

int zmk_asset_count(void) { return table_count > 0 ? table_count : 0; }

const struct zmk_asset_info *zmk_asset_get(int idx) {
    if (idx < 0 || idx >= zmk_asset_count()) {
        return NULL;
    }
    return &table[idx];
}

const struct zmk_asset_info *zmk_asset_find(const char *name) {
    for (int i = 0; i < zmk_asset_count(); i++) {
        if (strcmp(table[i].name, name) == 0) {
            return &table[i];
        }
    }
    return NULL;
}

static int probe(void) {
    struct file_header header;
    struct file_entry entries[ENTRY_MAX];
    uint32_t count;
    int err;

    if (!device_is_ready(flash_dev)) {
        LOG_ERR("flash controller not ready - assets offline");
        return -ENODEV;
    }

    err = read_at(0, &header, sizeof(header));
    if (err != 0) {
        LOG_ERR("asset partition read failed (err %d)", err);
        return err;
    }
    if (memcmp(header.magic, HEADER_MAGIC, sizeof(header.magic)) != 0) {
        LOG_INF("no packed asset image (or bad magic) - static theme fallback");
        return -ENOENT;
    }
    count = header.entry_count;
    if (count > ENTRY_MAX) {
        LOG_ERR("asset manifest claims %u entries (max %u) - ignoring image", count, ENTRY_MAX);
        return -EBADMSG;
    }
    if (count == 0) {
        LOG_INF("asset image is empty");
        return 0;
    }

    err = read_at(HEADER_SIZE, entries, count * ENTRY_SIZE);
    if (err != 0) {
        LOG_ERR("asset manifest read failed (err %d)", err);
        return err;
    }

    int kept = 0;
    uint32_t manifest_end = HEADER_SIZE + count * ENTRY_SIZE;

    for (uint32_t i = 0; i < count; i++) {
        struct file_entry *e = &entries[i];

        /* Reject anything that could make a reader run off the partition. */
        if (e->offset < manifest_end || e->len == 0 || e->offset > assets_size ||
            e->len > assets_size - e->offset || e->kind > ZMK_ASSET_LOTTIE ||
            strnlen(e->name, sizeof(e->name)) >= sizeof(e->name)) {
            LOG_WRN("asset '%s' out of bounds - skipped", e->name);
            continue;
        }
        struct zmk_asset_info *info = &table[kept++];

        memcpy(info->name, e->name, sizeof(info->name));
        info->name[sizeof(info->name) - 1] = '\0';
        info->offset = e->offset;
        info->len = e->len;
        info->kind = e->kind;
        info->fps = e->fps;

        if (info->fps > 15) {
            /* ~16 fps full-refresh ceiling (cobantab1a.overlay:151-169) */
            LOG_WRN("asset '%s' authored at %u fps exceeds the ~15 fps spec", info->name,
                    info->fps);
        }
    }

    table_count = kept;
    store_ready = kept > 0;
    LOG_INF("assets: %u entries (%s)", kept, store_ready ? "ready" : "none valid");
    return 0;
}

static int assets_init(void) {
    int err = probe();

    if (err && err != -ENOENT) {
        LOG_WRN("asset store offline (err %d)", err);
    }
    table_count = table_count < 0 ? 0 : table_count;
    return 0; /* never blocks boot: fallback is the static theme */
}

/* After the flash driver; before the display app work queue first
 * renders (APPLICATION + app priority). */
SYS_INIT(assets_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
