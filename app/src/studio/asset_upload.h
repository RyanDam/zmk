/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Studio `assets` bulk-upload backend: plain-C seam for
 * assets_subsystem.c (which owns the proto encoding); internal to src/studio/.
 * RPC thread only; flash touches capped at one manifest sector — multi-sector
 * work (blob writes, GC) runs on the uploader wq.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Return 0 / -errno; on business refusals *err points to a static RPC-safe string. */

int zmk_asset_upload_begin(const char *name, uint32_t size, uint32_t crc, uint16_t kind,
                           uint16_t fps, uint32_t *free_out, const char **err);

int zmk_asset_upload_data(uint32_t seq, const uint8_t *data, size_t len, uint32_t *accepted,
                          const char **err);

/* Blocks until the uploader has flushed, CRC-verified and committed; flush
 * drain bounded by the 16 KB staging ring. */
int zmk_asset_upload_end(uint32_t *free_out, bool *committed, const char **err);

int zmk_asset_upload_abort(void);

int zmk_asset_upload_status(bool *active, char *name, size_t name_sz, uint32_t *accepted);

/* Queues a compaction; returns immediately with the pre-GC free-space view. */
int zmk_asset_upload_gc(uint32_t *free_out);

/* Manifest rewrite omitting `name`; *deleted = false for builtin-only or
 * unknown names (caller maps that to an RPC error). */
int zmk_asset_store_delete(const char *name, bool *deleted);
