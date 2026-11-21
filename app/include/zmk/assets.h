/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Coban Tab 1a asset store v1 (backlog a12).
 *
 * Pad 10A played one host-uploaded GIF out of a hand-placed 100 KB flash
 * region behind the EEPROM (pad10a/config.h:62-71). Here the whole
 * `assets` partition (2 MB @ 0xda8000, carved from the unused OTA slot)
 * holds a packed image: a manifest header plus blobs, produced by
 * app/scripts/display-assets/pack_assets.py from a directory in
 * zmk-config. Host upload (a15) later rewrites the same image shape,
 * and h01's SD card would slot in as a second backend, so callers only
 * ever see this API.
 *
 * The image is validated once at boot: bad magic/version or an entry
 * pointing outside the partition yields an empty (or filtered) list and
 * one log line — the caller's fallback is the static theme, never a
 * crash. Reads go through the flash driver; a few hundred KB/s at
 * 40 MHz QSPI is plenty for a 15 fps asset spec.
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
    uint16_t kind; /* enum zmk_asset_kind               */
    uint16_t fps;  /* authored frame rate (0 = native)  */
};

/** Number of valid assets in the store (0 = absent/corrupt image). */
int zmk_asset_count(void);

/** Asset by list index; NULL if out of range. Pointer is static. */
const struct zmk_asset_info *zmk_asset_get(int idx);

/** Asset by exact name; NULL if missing. */
const struct zmk_asset_info *zmk_asset_find(const char *name);

/** Read @p len bytes of asset data from @p off. @retval 0 / -errno */
int zmk_asset_read(const struct zmk_asset_info *asset, uint32_t off, void *buf, uint32_t len);

/** True once a valid image has been parsed (drives "assets ready" UI). */
bool zmk_asset_ready(void);
