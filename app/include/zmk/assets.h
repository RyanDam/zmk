/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Coban Tab 1a asset store. The `assets` partition (2016 KiB @ 0xda8000) holds
 * one packed image — manifest header plus blobs — written by
 * app/scripts/display-assets/pack_assets.py and rewritten in place by the
 * Studio upload path.
 *
 * Formats (magic carries the version, shared with the pack script):
 *   ZMKAS001 (factory): read-only; header(16) + entries(32*n), blobs 4-aligned
 *     after, gen 0.
 *   ZMKAS002: header.flags = generation, trailing CRC32 over header+entries,
 *     each entry's `reserved` carries its blob CRC32. Blobs append into free
 *     gaps and the manifest is written LAST, so an interrupted upload cannot
 *     corrupt the live image: probe validates the manifest CRC or falls back to
 *     built-ins.
 *
 * Concurrency: the live table is rebuilt only by probe/reprobe under
 * `table_mutex`, and readers copy what they need under it. Long-lived holders
 * (anim's 'A:' file slots) watch zmk_assets_epoch() and drop handles when it
 * moves. Manifest commits never invalidate bytes an older reader is streaming;
 * GC runs between zmk_assets_request_quiesce() and its release.
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
    /** Built-in (firmware-embedded) assets: a memory-mapped pointer; NULL =
     *  partition-backed. zmk_asset_read() branches on it for the caller. */
    const uint8_t *rom;
};

/**
 * Built-in assets, embedded in the image and listed first by
 * zmk_asset_count()/get()/find(). A partition entry with the same name
 * overrides the built-in slot, so a host upload wins. Only exists with
 * CONFIG_ZMK_DISPLAY_ANIM_BUILTIN, and only the store reads it.
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

bool zmk_asset_ready(void);

/**
 * Probe now; safe to call twice, and it must run before any page builds — the
 * APPLICATION-priority SYS_INIT can land after the display work queue has
 * already built the restored home page, and those first opens would fail.
 */
void zmk_assets_probe_early(void);

/* Writable-store seams */

/** Table epoch, bumped on every rebuild; long-lived holders drop handles. */
uint32_t zmk_assets_epoch(void);

/** Manifest generation of the live image (0 = factory v1/absent). */
uint32_t zmk_assets_generation(void);

/** Rebuild the live table (seed + probe), bumping the epoch. Call on
 *  the display thread after quiescing players (anim tick) — never from
 *  the RPC thread while players are open. @retval 0 / -errno */
int zmk_assets_reprobe(void);

/** True when the partition holds a legacy v1 image: the next manifest write
 *  must GC-compact first (v2's manifest is 4 bytes larger). */
bool zmk_assets_legacy_image(void);

/** Largest contiguous free gap after the manifest floor (max upload size). */
int zmk_assets_free_bytes(uint32_t *out);

/** First-fit allocation of @p len (4-aligned) in the free gaps; marks nothing
 *  — one upload session at a time is the uploader's job. @retval 0 / -ENOSPC */
int zmk_assets_alloc(uint32_t len, uint32_t *off);

/** Snapshot of partition-backed entries (built-ins excluded); returns count. */
int zmk_assets_live_snapshot(struct zmk_asset_info *out, int max);

/** Validate + write a ZMKAS002 manifest (gen + 1, trailing CRC, per-entry
 *  CRCs) and raise zmk_assets_changed. Entries must be disjoint and at/after
 *  the v2 floor; a v1-shaped list returns -EAGAIN (caller GCs first). */
int zmk_assets_commit_entries(const struct zmk_asset_info *entries, int n);

/* Raw flash ops for the uploader/GC, range-guarded: 4-byte aligned writes,
 * 4 KiB sector erase per covered block. Uploader workqueue only. */
int zmk_assets_flash_write(uint32_t off, const void *buf, uint32_t len);
int zmk_assets_flash_erase(uint32_t off, uint32_t len);

/* Quiesce handshake for GC (blob relocation needs no open readers): the
 * requester sets the flag and raises zmk_assets_changed(RESET); the anim tick
 * closes all players and acks; release clears the flag and raises CHANGED so
 * the tick re-probes. The anim side refuses to build players while quiesced. */
int zmk_assets_request_quiesce(int timeout_ms); /* 0 = acked, -ETIMEDOUT */
void zmk_assets_release_quiesce(void);
bool zmk_assets_quiesced(void);

/* Display-thread ack after closing every player. false = the request already
 * timed out; do not stay parked. */
bool zmk_assets_quiesce_ack(void);
