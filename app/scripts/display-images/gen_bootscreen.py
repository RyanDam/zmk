#!/usr/bin/env python3
"""Generate the Coban Tab 1a boot splash image (LVGL 9 I2 + RLE C array).

Reads app/src/display/images/coban_bootscreen.png (480x320, the panel's
logical resolution) and writes app/src/display/images/coban_bootscreen.c:
a `lv_image_dsc_t` whose payload is an LVGL binary-image compression blob —
12-byte compress header + an RLE stream that decodes to
[4-entry palette][4 bpp-indexed pixel rows], exactly what the lv_bin_decoder
+ lv_rle combo in LVGL 9.5 expects (verified against
lv_bin_decoder.c:decode_compressed / decompress_image and lv_rle.c).

Why 2-bit: the art is black background + light artwork with grey
anti-aliased edges, so 4 grey levels (0/85/170/255) with Floyd-Steinberg
dithering are visually lossless here. Uncompressed I2 is 38.4 KB of flash;
the flat field RLEs down to a few KB (RLE runs on whole bytes, blk_size=1,
matching the decoder's `pixel_byte` for I2).

Requires Pillow. Rerun after editing the PNG:

    python3 app/scripts/display-images/gen_bootscreen.py
"""

import os
import struct
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
IMAGES = os.path.normpath(os.path.join(HERE, "..", "..", "src", "display", "images"))
SRC_PNG = os.path.join(IMAGES, "coban_bootscreen.png")
OUT_C = os.path.join(IMAGES, "coban_bootscreen.c")

W, H = 480, 320
LEVELS = (0, 85, 170, 255)  # I2 palette: 4 greys, index 0 = black
STRIDE = W // 4             # 2 bpp

# lv_image_compress_t: NONE=0, RLE=1 (src/draw/lv_image_dsc.h)
COMPRESS_RLE = 1


def quantise(png_path):
    """Composite on black, then FS-dither the grey ramp onto LEVELS."""
    im = Image.open(png_path)
    if im.size != (W, H):
        sys.exit(f"{png_path}: expected {W}x{H}, got {im.size}")
    rgba = im.convert("RGBA")
    black = Image.new("RGBA", (W, H), (0, 0, 0, 255))
    gray = Image.alpha_composite(black, rgba).convert("L")

    px = gray.load()
    indices = bytearray(W * H)

    def put(x, y, v):
        px[x, y] = max(0, min(255, int(v + 0.5)))

    for y in range(H):
        for x in range(W):
            v = px[x, y]
            # nearest level
            idx = min(range(4), key=lambda i: abs(LEVELS[i] - v))
            indices[y * W + x] = idx
            err = v - LEVELS[idx]
            # Floyd-Steinberg diffusion (classic 7/16-3/16-5/16-1/16)
            if x + 1 < W:
                put(x + 1, y, px[x + 1, y] + err * 7 / 16)
            if y + 1 < H:
                if x > 0:
                    put(x - 1, y + 1, px[x - 1, y + 1] + err * 3 / 16)
                put(x, y + 1, px[x, y + 1] + err * 5 / 16)
                if x + 1 < W:
                    put(x + 1, y + 1, px[x + 1, y + 1] + err * 1 / 16)
    return indices


def pack_rows(indices):
    """MSB-first 2bpp rows, stride 120 (decode_indexed_line I2 convention)."""
    rows = bytearray(STRIDE * H)
    for y in range(H):
        base = y * STRIDE
        for xb in range(STRIDE):
            p = y * W + xb * 4
            rows[base + xb] = (
                (indices[p] << 6) | (indices[p + 1] << 4) | (indices[p + 2] << 2) | indices[p + 3]
            )
    return rows


def payload_bytes(rows):
    """[palette][indexed]: what the RLE stream decompresses to.
    lv_color32_t stores B,G,R,A in memory order."""
    palette = b"".join(struct.pack("<4B", v, v, v, 255) for v in LEVELS)
    return palette + bytes(rows)


def rle_compress(data):
    """Encoder for the lv_rle.c format with blk_size=1:
    ctrl < 0x80 -> repeat the next byte ctrl times (max 127);
    ctrl & 0x80 -> copy the next (ctrl & 0x7f) bytes literally."""
    out = bytearray()
    i, n = 0, len(data)
    while i < n:
        run = 1
        while i + run < n and data[i + run] == data[i] and run < 0x7F:
            run += 1
        if run >= 3:
            out.append(run)
            out.append(data[i])
            i += run
        else:
            start = i
            while i < n and i - start < 0x7F:
                run = 1
                while i + run < n and data[i + run] == data[i] and run < 0x7F:
                    run += 1
                if run >= 3:
                    break
                i += 1
            out.append(0x80 | (i - start))
            out.extend(data[start:i])
    return bytes(out)


def rle_decompress_check(stream, expect):
    """Python port of lv_rle_decompress() to prove the stream round-trips."""
    out = bytearray()
    i = 0
    while i < len(stream):
        ctrl = stream[i]
        i += 1
        if ctrl & 0x80:
            cnt = ctrl & 0x7F
            out += stream[i : i + cnt]
            i += cnt
        else:
            out += bytes([stream[i]]) * ctrl
            i += 1
    if bytes(out) != expect:
        sys.exit("RLE round-trip FAILED against the decoder algorithm")


def main():
    indices = quantise(SRC_PNG)
    rows = pack_rows(indices)
    payload = payload_bytes(rows)
    stream = rle_compress(payload)
    rle_decompress_check(stream, payload)

    comp_hdr = struct.pack("<III", COMPRESS_RLE, len(stream), len(payload))
    blob = comp_hdr + stream

    name = "coban_bootscreen"
    lines = [
        "/*",
        " * Copyright (c) 2026 The ZMK Contributors",
        " *",
        " * SPDX-License-Identifier: MIT",
        " *",
        " * Boot splash art for the Coban Tab 1a (a08 splash, owner art 2026-11-23).",
        " * GENERATED by app/scripts/display-images/gen_bootscreen.py from",
        " * coban_bootscreen.png next to this file — do not edit, rerun the script.",
        " *",
        " * LVGL 9 binary image: cf=I2 (4 grey levels), flags=COMPRESSED with the",
        " * LVGL RLE method; data = 12-byte compress header + RLE stream decoding",
        " * to [16 B palette][38400 B indexed rows, stride 120]. Needs",
        " * LV_USE_RLE + LV_BIN_DECODER_RAM_LOAD (selected by ZMK_DISPLAY_SCREEN_MGR).",
        " */",
        "",
        "#include <lvgl.h>",
        "",
        "static const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST uint8_t",
        f"    {name}_map[] = {{",
    ]

    # 12-byte compress header then the RLE stream, 16 bytes per line
    all_bytes = list(blob)
    for off in range(0, len(all_bytes), 16):
        chunk = ", ".join(f"0x{b:02X}" for b in all_bytes[off : off + 16])
        lines.append(f"    {chunk},")
    lines += [
        "};",
        "",
        f"const lv_image_dsc_t {name} = {{",
        "    .header = {",
        "        .magic = LV_IMAGE_HEADER_MAGIC,",
        "        .cf = LV_COLOR_FORMAT_I2,",
        "        .flags = LV_IMAGE_FLAGS_COMPRESSED,",
        f"        .w = {W},",
        f"        .h = {H},",
        f"        .stride = {STRIDE},",
        "        .reserved_2 = 0,",
        "    },",
        f"    .data_size = sizeof({name}_map),",
        f"    .data = {name}_map,",
        "    .reserved = NULL,",
        "};",
        "",
    ]
    with open(OUT_C, "w") as f:
        f.write("\n".join(lines))

    print(f"payload {len(payload)} B -> RLE {len(stream)} B "
          f"({100.0 * len(stream) / len(payload):.1f} %), blob {len(blob)} B, "
          f"vs {W * H * 4} B if it were ARGB8888")
    print(f"wrote {OUT_C}")


if __name__ == "__main__":
    main()
