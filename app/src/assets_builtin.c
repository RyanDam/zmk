/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Built-in default animation asset (backlog a12, owner pass 2026-11-24).
 *
 * A device with a blank `assets` partition still animates: the default
 * GIF ships inside the firmware image. ESP32 flash is memory-mapped,
 * so "embedded in memory" is literal — the asset store serves the blob
 * straight from its mapped address through the same streaming API a
 * partition asset uses (assets.c branches on the rom pointer).
 *
 * The 2 MB GIF is embedded at build time with .incbin (path comes from
 * CMake as ZMK_ANIMBG_GIF_PATH); committing a generated ~13 MB C array
 * was never an option. Xtensa ELF prefixes C symbols with '_', so the
 * labels carry it explicitly.
 *
 * Bench art only: it is a fan-art clip, not ours to ship. Before any
 * product unit leaves the workshop it must be replaced with licensed
 * or in-house art of the same name (a12 decision note).
 */

#include <stddef.h>

#include <zephyr/sys/util.h>

#include <zmk/assets.h>

#define ZMK_STR2(x) #x
#define ZMK_STR(x) ZMK_STR2(x)

#ifndef ZMK_ANIMBG_GIF_PATH
#error "ZMK_ANIMBG_GIF_PATH must name the default GIF (see app/CMakeLists.txt)"
#endif

/* pushsection/popsection keep the compiler's own output where it
 * belongs; the section name matches *.rodata so it lands in the
 * image's mapped flash (DROM) region. */
__asm__(".pushsection .rodata.zmk_builtin_animbg, \"a\", @progbits\n\t"
        ".balign 4\n\t"
        ".globl _zmk_builtin_animbg_start\n\t"
        "_zmk_builtin_animbg_start:\n\t"
        ".incbin \"" ZMK_STR(ZMK_ANIMBG_GIF_PATH) "\"\n\t"
        ".globl _zmk_builtin_animbg_end\n\t"
        "_zmk_builtin_animbg_end:\n\t"
        ".popsection\n\t");

extern char _zmk_builtin_animbg_start[];
extern char _zmk_builtin_animbg_end[];

#define ZMK_ANIMBG_GIF_LEN (_zmk_builtin_animbg_end - _zmk_builtin_animbg_start)

const struct zmk_asset_info *zmk_assets_builtin_table(int *count) {
    /* One built-in asset today ("animbg", the store's name for "the"
     * animation: the standalone page plays it first and it is the
     * default background). fps 0 = play at the GIF's own timing; kind
     * is fixed by the file we embed. len/rom come from the linker —
     * not compile-time constants, so the table is filled on first
     * call (the store probes once, at boot, single-threaded). */
    static struct zmk_asset_info table[] = {
        {
            .name = "animbg",
            .offset = 0, /* unused: rom-backed */
            .kind = ZMK_ASSET_GIF,
            .fps = 0,
        },
    };

    table[0].rom = (const uint8_t *)_zmk_builtin_animbg_start;
    table[0].len = (uint32_t)(_zmk_builtin_animbg_end - _zmk_builtin_animbg_start);

    if (count != NULL) {
        *count = ARRAY_SIZE(table);
    }

    return table;
}
