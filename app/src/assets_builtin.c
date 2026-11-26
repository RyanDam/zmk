/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Built-in default animation asset: served from mapped flash through the
 * same streaming API a partition asset uses. Embedded via .incbin (CMake:
 * ZMK_ANIMBG_GIF_PATH); Xtensa ELF prefixes C symbols with '_'. Bench art
 * only: replace with licensed art before product ships.
 */

#include <stddef.h>

#include <zephyr/sys/util.h>

#include <zmk/assets.h>

#define ZMK_STR2(x) #x
#define ZMK_STR(x) ZMK_STR2(x)

#ifndef ZMK_ANIMBG_GIF_PATH
#error "ZMK_ANIMBG_GIF_PATH must name the default GIF (see app/CMakeLists.txt)"
#endif

/* .incbin in .rodata.* so the blob lands in the mapped flash (DROM) region. */
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
    /* fps 0 = the GIF's own timing; len/rom come from the linker, so the
     * table is filled on first (boot-only) call. */
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
