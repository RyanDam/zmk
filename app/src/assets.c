/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Asset store implementation (backlog a12; writable in a15). See
 * zmk/assets.h for the format and the concurrency model.
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
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <zmk/assets.h>
#include <zmk/events/assets_changed.h>

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
#define HEADER_MAGIC_V1 "ZMKAS001"
#define HEADER_MAGIC_V2 "ZMKAS002"
#define HEADER_SIZE 16
#define ENTRY_SIZE 32 /* == sizeof(struct file_entry), asserted below */
#define ENTRY_MAX 32

/* a15: blobs live in 4 KiB sectors above the manifest sector so an
 * erase never clobbers a neighbour; the manifest (<= 1 KiB) always
 * fits sector 0. Constant data floor keeps probe/alloc/GC honest no
 * matter how many entries a generation carries. */
#define DATA_FLOOR 4096
#define SECTOR_SIZE 4096

struct __packed file_header {
    uint8_t magic[8];
    uint32_t entry_count;
    uint32_t flags; /* v2: manifest generation; v1: unused (0) */
};

struct __packed file_entry {
    char name[ZMK_ASSET_NAME_MAX];
    uint32_t offset;
    uint32_t len;
    uint16_t kind;
    uint16_t fps;
    uint32_t reserved; /* v2: blob CRC32; v1: 0 */
};

/* Wire format is shared with scripts/display-assets/pack_assets.py; a
 * layout drift here silently rejects the first blob (manifest_end would
 * claim the manifest is larger than it is). */
BUILD_ASSERT(sizeof(struct file_entry) == ENTRY_SIZE);
BUILD_ASSERT(sizeof(struct file_header) == HEADER_SIZE);
BUILD_ASSERT(DATA_FLOOR >= HEADER_SIZE + ENTRY_MAX * ENTRY_SIZE + 4);

static struct zmk_asset_info table[ENTRY_MAX];
static int table_count = -1; /* -1 until probed */
static bool store_ready;

/* a15 state (see header docs) */
static struct k_mutex table_lock;
static uint32_t table_epoch;
static uint32_t live_gen;
static bool legacy_image;
static atomic_t quiesce_req;    /* requester wants playback parked   */
static atomic_t quiesce_active; /* anim parked and acked             */
static struct k_sem quiesce_ack;

static int read_at(uint32_t off, void *buf, uint32_t len) {
    if (off > assets_size || len > assets_size - off) {
        return -EINVAL;
    }
    return flash_read(flash_dev, assets_base + off, buf, len);
}

/* ---- a15 guarded raw ops (uploader/GC only, aligned contracts) ---- */

int zmk_assets_flash_write(uint32_t off, const void *buf, uint32_t len) {
    if (len == 0 || (off & 3) != 0 || (len & 3) != 0 || off > assets_size ||
        len > assets_size - off) {
        return -EINVAL; /* 4-byte aligned, in-partition only */
    }
    return flash_write(flash_dev, assets_base + off, buf, len);
}

int zmk_assets_flash_erase(uint32_t off, uint32_t len) {
    if (off % SECTOR_SIZE != 0 || len % SECTOR_SIZE != 0 || off > assets_size ||
        len > assets_size - off) {
        return -EINVAL; /* sector-aligned, in-partition only */
    }
    return flash_erase(flash_dev, assets_base + off, len);
}

/* ---- read API (all snapshot under the table lock) ----------------- */

int zmk_asset_read(const struct zmk_asset_info *asset, uint32_t off, void *buf, uint32_t len) {
    struct zmk_asset_info snap;

    if (asset == NULL || buf == NULL || table_count < 0) {
        return -EINVAL;
    }

    k_mutex_lock(&table_lock, K_FOREVER);
    snap = *asset; /* entries live in the static table; contents move */
    k_mutex_unlock(&table_lock);

    if (off > snap.len || len > snap.len - off) {
        return -EINVAL;
    }
    if (snap.rom != NULL) {
        /* Built-in asset: mapped flash inside the app image. */
        memcpy(buf, snap.rom + off, len);
        return 0;
    }
    return read_at(snap.offset + off, buf, len);
}

bool zmk_asset_ready(void) { return store_ready; }

int zmk_asset_count(void) {
    int n;

    k_mutex_lock(&table_lock, K_FOREVER);
    n = table_count > 0 ? table_count : 0;
    k_mutex_unlock(&table_lock);
    return n;
}

const struct zmk_asset_info *zmk_asset_get(int idx) {
    const struct zmk_asset_info *ret = NULL;

    k_mutex_lock(&table_lock, K_FOREVER);
    if (idx >= 0 && idx < (table_count > 0 ? table_count : 0)) {
        ret = &table[idx];
    }
    k_mutex_unlock(&table_lock);
    return ret;
}

const struct zmk_asset_info *zmk_asset_find(const char *name) {
    const struct zmk_asset_info *ret = NULL;

    k_mutex_lock(&table_lock, K_FOREVER);
    for (int i = 0; i < (table_count > 0 ? table_count : 0); i++) {
        if (strcmp(table[i].name, name) == 0) {
            ret = &table[i];
            break;
        }
    }
    k_mutex_unlock(&table_lock);
    return ret;
}

uint32_t zmk_assets_epoch(void) { return table_epoch; }

uint32_t zmk_assets_generation(void) { return live_gen; }

bool zmk_assets_legacy_image(void) { return legacy_image; }

/* Name lookup inside the live table; @p n bounds it (merge in progress).
 * Callers hold table_lock. */
static int table_find_locked(const char *name, int n) {
    for (int i = 0; i < n; i++) {
        if (strcmp(table[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

/* Built-in (firmware-embedded) assets first, so every read path works
 * even with a blank/corrupt partition — see zmk/assets.h.
 * Callers hold table_lock. */
static int table_seed_builtin(void) {
    int n = 0;

#if IS_ENABLED(CONFIG_ZMK_DISPLAY_ANIM_BUILTIN)
    int bn = 0;
    const struct zmk_asset_info *bt = zmk_assets_builtin_table(&bn);

    for (int i = 0; i < bn && n < ENTRY_MAX; i++) {
        if (bt[i].rom == NULL || bt[i].len == 0) {
            LOG_WRN("built-in '%s' has no data - skipped", bt[i].name);
            continue;
        }
        table[n++] = bt[i];
    }
    if (n > 0) {
        LOG_INF("built-in assets: %d", n);
    }
#endif

    return n;
}

/* Read + validate the manifest, merge partition entries into the live
 * table. Callers hold table_lock (so a concurrent reader never sees a
 * half-merged table); flash reads inside are mutex-free by design —
 * the driver serializes them. */
static int probe_locked(void) {
    struct file_header header;
    struct file_entry entries[ENTRY_MAX];
    uint32_t crc_tail = 0, crc_calc;
    uint32_t count;
    bool v2;
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
    if (memcmp(header.magic, HEADER_MAGIC_V2, sizeof(header.magic)) == 0) {
        v2 = true;
        legacy_image = false;
    } else if (memcmp(header.magic, HEADER_MAGIC_V1, sizeof(header.magic)) == 0) {
        v2 = false;
        legacy_image = true; /* first commit must GC onto the v2 floor */
    } else {
        LOG_INF("no packed asset image (or bad magic) - built-in/static fallback");
        return -ENOENT;
    }

    count = header.entry_count;
    if (count > ENTRY_MAX) {
        LOG_ERR("asset manifest claims %u entries (max %u) - ignoring image", count, ENTRY_MAX);
        return -EBADMSG;
    }

    if (count > 0) {
        err = read_at(HEADER_SIZE, entries, count * ENTRY_SIZE);
        if (err != 0) {
            LOG_ERR("asset manifest read failed (err %d)", err);
            return err;
        }
    }

    if (v2) {
        /* Trailing CRC covers header + entries (see pack script). */
        err = read_at(HEADER_SIZE + count * ENTRY_SIZE, &crc_tail, sizeof(crc_tail));
        if (err != 0) {
            return err;
        }
        crc_calc = crc32_ieee_update(0, (const uint8_t *)&header, sizeof(header));
        crc_calc = crc32_ieee_update(crc_calc, (const uint8_t *)entries, count * ENTRY_SIZE);
        if (crc_calc != crc_tail) {
            /* Torn commit — the ONLY thing a commit writes last, so
             * reaching here means a write died mid-sector: fall back
             * to built-ins, never trust a byte of this generation. */
            LOG_ERR("manifest CRC mismatch (live gen %u) - built-in/static fallback",
                    header.flags);
            return -EBADMSG;
        }
        live_gen = header.flags;
    } else {
        live_gen = 0;
    }

    if (count == 0) {
        LOG_INF("asset image is empty");
        return 0;
    }

    uint32_t manifest_end = v2 ? DATA_FLOOR : HEADER_SIZE + count * ENTRY_SIZE;
    int merged = 0;

    for (uint32_t i = 0; i < count; i++) {
        struct file_entry *e = &entries[i];

        /* Reject anything that could make a reader run off the partition. */
        if (e->offset < manifest_end || e->len == 0 || e->offset > assets_size ||
            e->len > assets_size - e->offset || e->kind > ZMK_ASSET_LOTTIE ||
            strnlen(e->name, sizeof(e->name)) >= sizeof(e->name)) {
            LOG_WRN("asset '%s' out of bounds - skipped", e->name);
            continue;
        }

        /* Partition wins by name: override the built-in slot in place,
         * so a host upload replaces the factory default. */
        int slot = table_find_locked(e->name, table_count);

        if (slot < 0) {
            if (table_count >= ENTRY_MAX) {
                LOG_WRN("asset table full (%u) - skipping '%s'", ENTRY_MAX, e->name);
                continue;
            }
            slot = table_count++;
        } else if (table[slot].rom != NULL) {
            LOG_INF("asset '%s' overrides built-in", e->name);
        }

        struct zmk_asset_info *info = &table[slot];

        memcpy(info->name, e->name, sizeof(info->name));
        info->name[sizeof(info->name) - 1] = '\0';
        info->offset = e->offset;
        info->len = e->len;
        info->kind = e->kind;
        info->fps = e->fps;
        info->crc = v2 ? e->reserved : 0;
        info->epoch = table_epoch;
        info->rom = NULL;
        merged++;

        if (info->fps > 15) {
            /* ~16 fps full-refresh ceiling (cobantab1a.overlay:151-169) */
            LOG_WRN("asset '%s' authored at %u fps exceeds the ~15 fps spec", info->name,
                    info->fps);
        }
    }

    LOG_INF("partition: %u entries merged, store holds %d (gen %u%s)", merged, table_count,
            live_gen, legacy_image ? ", legacy v1" : "");
    return 0;
}

/* Full table rebuild. Locking: one mutex window covers seed + merge,
 * so readers holding pointers get complete old-or-new entries, never
 * a torn one; the epoch bump tells long holders (anim) to re-open. */
static int assets_rebuild(void) {
    int err;

    k_mutex_lock(&table_lock, K_FOREVER);
    table_epoch++;
    table_count = table_seed_builtin();
    err = probe_locked();
    if (err && err != -ENOENT) {
        LOG_WRN("asset store offline (err %d) - %d built-in assets remain", err, table_count);
    }
    store_ready = table_count > 0;
    k_mutex_unlock(&table_lock);
    return err;
}

/* ---- a15: free-space view + allocation ---------------------------- */

struct blob {
    uint32_t off;
    uint32_t end; /* offset + ROUND_UP(len) — the space it occupies */
};

static int blob_cmp(const void *pa, const void *pb) {
    const struct blob *a = pa, *b = pb;

    return a->off < b->off ? -1 : a->off > b->off ? 1 : 0;
}

/* Collect the space each partition-backed entry occupies. Callers
 * supply the destination; returns the count. */
static int blobs_live(struct blob *out, int max) {
    int n = 0;

    k_mutex_lock(&table_lock, K_FOREVER);
    for (int i = 0; i < (table_count > 0 ? table_count : 0) && n < max; i++) {
        if (table[i].rom != NULL || table[i].len == 0) {
            continue;
        }
        out[n].off = table[i].offset;
        out[n].end = ROUND_UP(table[i].offset + table[i].len, SECTOR_SIZE);
        n++;
    }
    k_mutex_unlock(&table_lock);

    /* sort (probe merge order need not be by offset) */
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            if (blob_cmp(&out[j], &out[i]) < 0) {
                struct blob t = out[i];

                out[i] = out[j];
                out[j] = t;
            }
        }
    }
    return n;
}

/* Largest free run in [DATA_FLOOR, assets_size) not covered by blobs,
 * sector-granular. */
static uint32_t largest_gap(const struct blob *blobs, int n, uint32_t *gap_at) {
    uint32_t cursor = DATA_FLOOR, best = 0, best_at = DATA_FLOOR;

    for (int i = 0; i < n; i++) {
        if (blobs[i].off > cursor) {
            uint32_t run = blobs[i].off - cursor;

            if (run > best) {
                best = run;
                best_at = cursor;
            }
        }
        if (blobs[i].end > cursor) {
            cursor = blobs[i].end;
        }
    }
    if (assets_size > cursor && assets_size - cursor > best) {
        best = assets_size - cursor;
        best_at = cursor;
    }
    if (gap_at != NULL) {
        *gap_at = best_at;
    }
    return best > assets_size - DATA_FLOOR ? assets_size - DATA_FLOOR : best;
}

int zmk_assets_free_bytes(uint32_t *out) {
    struct blob blobs[ENTRY_MAX];
    int n = blobs_live(blobs, ENTRY_MAX);

    *out = largest_gap(blobs, n, NULL);
    return 0;
}

int zmk_assets_alloc(uint32_t len, uint32_t *off) {
    struct blob blobs[ENTRY_MAX];
    int n = blobs_live(blobs, ENTRY_MAX);
    uint32_t gap_at;
    uint32_t need = ROUND_UP(len, SECTOR_SIZE);

    /* First fit (largest-gap when no single gap fits is pointless). */
    uint32_t cursor = DATA_FLOOR;

    for (int i = 0; i <= n; i++) {
        uint32_t stop = (i < n) ? blobs[i].off : assets_size;

        if (stop >= cursor + need && stop - cursor >= need) {
            gap_at = cursor;
            *off = gap_at;
            return 0;
        }
        if (i < n && blobs[i].end > cursor) {
            cursor = blobs[i].end;
        }
    }
    return -ENOSPC;
}

int zmk_assets_live_snapshot(struct zmk_asset_info *out, int max) {
    int n = 0;

    k_mutex_lock(&table_lock, K_FOREVER);
    for (int i = 0; i < (table_count > 0 ? table_count : 0) && n < max; i++) {
        if (table[i].rom != NULL) {
            continue;
        }
        out[n++] = table[i];
    }
    k_mutex_unlock(&table_lock);
    return n;
}

/* ---- a15: manifest commit ------------------------------------------ */

static struct __packed {
    struct file_header header;
    struct file_entry entries[ENTRY_MAX];
    uint32_t crc_tail;
} commit_buf;

/* Uploads commit on the uploader queue while deletes commit on the RPC
 * thread: one lock covers the shared commit_buf, the generation
 * counter and the manifest sector itself. */
static struct k_mutex commit_lock;

int zmk_assets_commit_entries(const struct zmk_asset_info *entries, int n) {
    uint32_t crc;
    int err;

    if (n < 0 || n > ENTRY_MAX || entries == NULL) {
        return -EINVAL;
    }

    /* Validate the full list — bounds, mutual disjointness, floor. A
     * list carrying v1-era blobs below DATA_FLOOR is refused with
     * -EAGAIN: the caller GC-compacts first (the empty/upgraded list
     * GC commits then passes this same gate, which is exactly the
     * v1 -> v2 upgrade). */
    for (int i = 0; i < n; i++) {
        const struct zmk_asset_info *e = &entries[i];

        if (e->rom != NULL || e->len == 0 || strnlen(e->name, sizeof(e->name)) >= sizeof(e->name) ||
            e->kind > ZMK_ASSET_LOTTIE || e->offset > assets_size ||
            e->len > assets_size - e->offset) {
            return -EINVAL;
        }
        if (e->offset < DATA_FLOOR) {
            return -EAGAIN;
        }
        for (int j = i + 1; j < n; j++) {
            uint32_t a_end = e->offset + e->len, b_end = entries[j].offset + entries[j].len;

            if (e->offset < b_end && entries[j].offset < a_end) {
                return -EADDRNOTAVAIL; /* overlapping blobs: caller must GC */
            }
        }
    }

    k_mutex_lock(&commit_lock, K_FOREVER);

    commit_buf.header = (struct file_header){0};
    memcpy(commit_buf.header.magic, HEADER_MAGIC_V2, sizeof(commit_buf.header.magic));
    commit_buf.header.entry_count = n;
    commit_buf.header.flags = live_gen + 1; /* generation = commit counter */

    for (int i = 0; i < n; i++) {
        struct file_entry *out = &commit_buf.entries[i];

        memset(out, 0, sizeof(*out));
        memcpy(out->name, entries[i].name, strnlen(entries[i].name, ZMK_ASSET_NAME_MAX));
        out->offset = entries[i].offset;
        out->len = entries[i].len;
        out->kind = entries[i].kind;
        out->fps = entries[i].fps;
        out->reserved = entries[i].crc;
    }

    crc = crc32_ieee_update(0, (const uint8_t *)&commit_buf.header, sizeof(commit_buf.header));
    crc = crc32_ieee_update(crc, (const uint8_t *)commit_buf.entries,
                            n * sizeof(struct file_entry));
    commit_buf.crc_tail = crc;

    err = flash_erase(flash_dev, assets_base, SECTOR_SIZE);
    if (err != 0) {
        LOG_ERR("manifest erase failed (err %d) - live image untouched", err);
        k_mutex_unlock(&commit_lock);
        return err;
    }
    err = flash_write(flash_dev, assets_base, &commit_buf,
                      sizeof(commit_buf.header) + n * sizeof(struct file_entry) + 4);
    if (err != 0) {
        LOG_ERR("manifest write failed (err %d) - probe will fall back", err);
        k_mutex_unlock(&commit_lock);
        return err;
    }

    live_gen = commit_buf.header.flags;
    k_mutex_unlock(&commit_lock);
    LOG_INF("manifest committed: generation %u, %d entries", live_gen, n);

    raise_zmk_assets_changed((struct zmk_assets_changed){.reason = ZMK_ASSETS_CHANGED});
    return 0;
}

void zmk_assets_probe_early(void) { (void)assets_rebuild(); }

int zmk_assets_reprobe(void) { return assets_rebuild(); }

/* ---- a15: quiesce handshake (GC support) --------------------------- */

int zmk_assets_request_quiesce(int timeout_ms) {
    atomic_set(&quiesce_req, 1);
    raise_zmk_assets_changed((struct zmk_assets_changed){.reason = ZMK_ASSETS_QUIESCE});
    if (k_sem_take(&quiesce_ack, K_MSEC(timeout_ms)) != 0) {
        atomic_set(&quiesce_req, 0);
        return -ETIMEDOUT;
    }
    return 0;
}

void zmk_assets_release_quiesce(void) {
    atomic_set(&quiesce_active, 0);
    atomic_set(&quiesce_req, 0);
    raise_zmk_assets_changed((struct zmk_assets_changed){.reason = ZMK_ASSETS_CHANGED});
}

bool zmk_assets_quiesced(void) {
    /* True for the whole GC window: from the request (before the ack)
     * through the compaction, so a delete committing on the RPC thread
     * never races a GC manifest rewrite. */
    return atomic_get(&quiesce_active) != 0 || atomic_get(&quiesce_req) != 0;
}

/* Called by the display thread once it has closed every player
 * (anim_tick consumes a QUIESCE request). @retval true the request was
 * still live; false the requester already timed out — the caller must
 * NOT stay parked. */
bool zmk_assets_quiesce_ack(void) {
    if (atomic_get(&quiesce_req) == 0) {
        return false; /* stale: the GC gave up before we parked */
    }
    atomic_set(&quiesce_req, 0);
    atomic_set(&quiesce_active, 1);
    k_sem_give(&quiesce_ack);
    return true;
}

/* After the flash driver; before the display app work queue first
 * renders (APPLICATION + app priority). The display main also calls
 * zmk_assets_probe_early() before building pages — persisted-home
 * restoration can create a page (and its first gif open) before
 * this init runs, and an unprobed store answers "no assets" to
 * everything (bench 2026-11-24: home=anim page booted black). */
static int assets_init_hook(void) {
    k_mutex_init(&table_lock);
    k_mutex_init(&commit_lock);
    k_sem_init(&quiesce_ack, 0, 1);
    (void)assets_rebuild();
    return 0; /* never blocks boot: fallback is the static theme */
}

SYS_INIT(assets_init_hook, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
