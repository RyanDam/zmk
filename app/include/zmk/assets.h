/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Coban Tab 1a asset store (backlog a12, writable in a15).
 *
 * Pad 10A played one host-uploaded GIF out of a hand-placed 100 KB flash
 * region behind the EEPROM (pad10a/config.h:62-71). Here the whole
 * `assets` partition (2016 KiB @ 0xda8000, carved from the unused OTA
 * slot) holds a packed image: a manifest header plus blobs, produced by
 * app/scripts/display-assets/pack_assets.py from a directory in
 * zmk-config, and rewritten in place by the a15 Studio upload path.
 * h01's SD card would slot in as a second backend, so callers only ever
 * see this API.
 *
 * Image formats (magic carries the version, shared with pack_assets.py):
 *   ZMKAS001 (factory, a12): read-only. header(16) + entries(32*n);
 *     blobs 4-aligned after. header.flags unused (gen 0).
 *   ZMKAS002 (a15): header.flags = generation (bumped per commit);
 *     trailing CRC32 after the entries covers header+entries; each
 *     entry's `reserved` carries the blob CRC32. Blobs are append-only
 *     into free gaps, the manifest is written LAST (commit-last): a torn
 *     or interrupted upload can never corrupt the live image — probe
 *     either validates the manifest CRC or falls back to built-ins.
 *
 * Concurrency model (a15): the live table is rebuilt only by probe
 * (boot, or the display-thread reprobe after a commit), guarded by
 * `table_mutex`; every reader copies what it needs under the mutex.
 * Consumers holding long-lived references (anim's 'A:' file slots) must
 * check zmk_assets_epoch() — bumped on every table rebuild — and drop
 * their handles when it moves. Manifest-only commits (upload/delete)
 * never invalidate bytes a pre-epoch reader streams (old blobs stay
 * valid until GC); GC itself must run between
 * zmk_assets_request_quiesce() and its release, i.e. with no players.
 *
 * Reads go through the flash driver; a few hundred KB/s at 40 MHz QSPI
 * is plenty for a 15 fps asset spec. Writes/erasures run through the
 * same driver and are safe from any thread (ESP HAL: IRAM + cache
 * windows + yield scheduling), but keep them off the RPC and display
 * threads — see studio/asset_upload.c.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define ZMK_ASSET_NAME_MAX 16

/** Asset kinds, matching the pack script's extension mapping. */
enum zmk_asset_kind {
    ZMK_ASSET_GIF = 0,
    ZMK_ASSET_LOTTIE = 1, /* .json, ThorVG/Lottie */
};

struct zmk_asset_info {
    char name[ZMK_ASSET_NAME_MAX]; /* without extension, e.g. "animbg"  */
    uint32_t offset;               /* byte offset inside the partition  */
    uint32_t len;
    uint16_t kind; /* enum zmk_asset_kind                               */
    uint16_t fps;  /* authored frame rate (0 = native)                  */
    uint32_t crc;  /* blob CRC32 from the manifest (0 = unknown, v1)    */
    uint32_t epoch; /* table epoch stamped at probe; guard long holders  */
    /** Non-NULL for built-in (firmware-embedded) assets: a
     *  memory-mapped pointer, served straight from the app image.
     *  NULL = partition-backed; the zmk_asset_read() seam branches
     *  on it, so callers never care where a byte comes from. */
    const uint8_t *rom;
};

/**
 * Built-in asset table (a12): assets embedded in the firmware image,
 * listed first by zmk_asset_count()/get()/find(). Entries in the
 * `assets` partition with the same name override the built-in slot,
 * so a host upload always wins over the factory default. The table
 * exists only with CONFIG_ZMK_DISPLAY_ANIM_BUILTIN; the store seeds
 * itself from it, callers go through the normal API.
 */
const struct zmk_asset_info *zmk_assets_builtin_table(int *count);

/** Number of valid assets in the store (0 = absent/corrupt image). */
int zmk_asset_count(void);

/** Asset by list index; NULL if out of range. Pointer is static:
 *  short-lived use only — holders must watch zmk_assets_epoch(). */
const struct zmk_asset_info *zmk_asset_get(int idx);

/** Asset by exact name; NULL if missing. Same pointer caveat as get(). */
const struct zmk_asset_info *zmk_asset_find(const char *name);

/** Read @p len bytes of asset data from @p off. @retval 0 / -errno */
int zmk_asset_read(const struct zmk_asset_info *asset, uint32_t off, void *buf, uint32_t len);

/** True once at least one asset is available (partition image or
 *  built-in) — drives the "assets ready" UI. */
bool zmk_asset_ready(void);

/**
 * Probe the store right now (idempotent enough for the display main's
 * pre-page-build call: a second pass re-reads the partition and
 * re-merges; nobody has open handles that early in boot). The SYS_INIT
 * at APPLICATION priority can land after the display work queue has
 * already built the restored home page — first gif opens would then
 * see an unprobed store and fail.
 */
void zmk_assets_probe_early(void);

/* ------------------------------------------------------------------ */
/* a15: writable-store seams                                           */
/* ------------------------------------------------------------------ */

/** Table epoch: bumped on every rebuild (probe/reprobe). Long-lived
 *  holders (anim's file slots) must drop their handles when it moves. */
uint32_t zmk_assets_epoch(void);

/** Manifest generation of the live image (0 = factory v1/absent). */
uint32_t zmk_assets_generation(void);

/** Rebuild the live table (seed + probe), bumping the epoch. Call on
 *  the display thread after quiescing players (anim tick) — never from
 *  the RPC thread while players are open. @retval 0 / -errno */
int zmk_assets_reprobe(void);

/** True when the partition holds a legacy v1 image: the next
 *  manifest-writing operation must GC-compact first (v2's manifest is
 *  4 bytes larger and the trailing CRC shifts the blob floor). */
bool zmk_assets_legacy_image(void);

/** Largest contiguous free gap between manifest floor and the
 *  partition end (what an upload of that size fits). */
int zmk_assets_free_bytes(uint32_t *out);

/** First-fit allocation of @p len (4-aligned) inside the free gaps of
 *  the live image. Does not mark anything — a single upload session at
 *  a time is enforced by the uploader, which passes the final list to
 *  commit_entries(). @retval 0 / -ENOSPC */
int zmk_assets_alloc(uint32_t len, uint32_t *off);

/** Snapshot of partition-backed entries (built-ins excluded — they are
 *  firmware, not manifest content). Returns the count. */
int zmk_assets_live_snapshot(struct zmk_asset_info *out, int max);

/** Validate + write a ZMKAS002 manifest (generation = live gen + 1,
 *  trailing CRC, per-entry CRCs in `reserved`), then raise
 *  zmk_assets_changed so the display re-probes. Entries must be
 *  in-bounds, mutually disjoint, at/after the v2 manifest floor —
 *  a v1-shaped list returns -EAGAIN (caller GCs first). Never touches
 *  blob bytes. @retval 0 / -errno */
int zmk_assets_commit_entries(const struct zmk_asset_info *entries, int n);

/* Raw in-partition flash ops for the uploader/GC, range-guarded to
 * [0, partition size): writes must be 4-byte aligned; erase is
 * sector-erased per covered 4 KiB block (blob floors align it right).
 * Call only from the uploader workqueue — never RPC/display. */
int zmk_assets_flash_write(uint32_t off, const void *buf, uint32_t len);
int zmk_assets_flash_erase(uint32_t off, uint32_t len);

/* Quiesce handshake for GC (blob relocation needs zero open readers):
 * the requester sets the flag and raises zmk_assets_changed(RESET);
 * the anim tick closes all players and acks (the wait here returns);
 * release clears the flag and raises CHANGED so the tick re-probes and
 * rebuilds. The anim side refuses to (re)build players while quiesced. */
int zmk_assets_request_quiesce(int timeout_ms); /* 0 = acked, -ETIMEDOUT */
void zmk_assets_release_quiesce(void);
bool zmk_assets_quiesced(void);

/* Display-thread ack after closing every player (anim tick consumes a
 * QUIESCE request by calling this). false = the request already timed
 * out; the caller must not stay parked. */
bool zmk_assets_quiesce_ack(void);
