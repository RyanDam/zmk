/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Studio `assets` bulk-upload backend (backlog a15). Plain-C seam for
 * assets_subsystem.c (which owns the proto encoding); internal to
 * src/studio/. See asset_upload.c for the integrity model.
 *
 * All entry points run on the RPC thread and never touch flash beyond
 * what a single manifest sector costs; every multi-sector operation
 * (blob writes, GC) runs on the uploader workqueue.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* All functions return 0 / -errno. On business refusals (name bad, no
 * session, seq gap) *err points to a static, RPC-safe human string. */

int zmk_asset_upload_begin(const char *name, uint32_t size, uint32_t crc, uint16_t kind,
                           uint16_t fps, uint32_t *free_out, const char **err);

int zmk_asset_upload_data(uint32_t seq, const uint8_t *data, size_t len, uint32_t *accepted,
                          const char **err);

/* Blocks the RPC thread until the uploader has flushed, CRC-verified
 * and committed (or failed): the commit is one manifest sector, but
 * the flush drain is bounded by the 16 KB staging ring. */
int zmk_asset_upload_end(uint32_t *free_out, bool *committed, const char **err);

int zmk_asset_upload_abort(void);

int zmk_asset_upload_status(bool *active, char *name, size_t name_sz, uint32_t *accepted);

/* Queues a compaction on the uploader queue; returns immediately with
 * the pre-GC view of free space (progress notifications carry the
 * rest). */
int zmk_asset_upload_gc(uint32_t *free_out);

/* Manifest rewrite omitting `name` (a15 delete). Returns *deleted =
 * false for builtin-only/unknown names without erroring the commit
 * path; the caller maps that to an RPC error. */
int zmk_asset_store_delete(const char *name, bool *deleted);
