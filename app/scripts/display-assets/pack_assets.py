#!/usr/bin/env python3
"""Pack a directory of dev art into the Coban Tab 1a asset store image
(workspace backlog a12).

    ./pack_assets.py <assets-dir> <out.bin> [--partition-size 0x200000]

The image layout must match app/src/assets.c ("keep the two in sync"):

    header : b"ZMKAS001" | entry_count u32le | flags u32le          (16 B)
    entry  : name[16] | offset u32 | len u32 | kind u16 | fps u16
             | reserved u32                                          (40 B)
    blobs  : file data, 4-byte aligned, after the manifest

Asset name = filename without extension; an optional "@fps" suffix
("meadow@12.gif") records the authored frame rate (the firmware warns
above 15 fps, the panel's ~16 fps full-refresh ceiling). Extensions:
.gif -> kind 0, .json -> kind 1 (Lottie). Flash the result at the
`assets` partition offset (0xda8000 on cobantab1a, see the board DTS).
"""

import argparse
import struct
import sys
from pathlib import Path

MAGIC = b"ZMKAS001"
HEADER = struct.Struct("<8sII")
ENTRY = struct.Struct("<16sIIHHI")
KINDS = {".gif": 0, ".json": 1}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("assets_dir", type=Path)
    ap.add_argument("out", type=Path)
    ap.add_argument("--partition-size", type=lambda s: int(s, 0), default=0x200000)
    ap.add_argument("--offset", type=lambda s: int(s, 0), default=0xDA8000,
                    help="partition offset, for the printed flash command")
    args = ap.parse_args()

    files = sorted(p for p in args.assets_dir.iterdir()
                   if p.is_file() and p.suffix.lower() in KINDS)
    if not files:
        print(f"no .gif/.json assets in {args.assets_dir}", file=sys.stderr)

    names = {}
    entries, blobs = [], []
    manifest_end = HEADER.size + ENTRY.size * len(files)
    pos = (manifest_end + 3) & ~3

    for path in files:
        stem = path.stem
        fps = 15
        if "@" in stem:
            stem, _, fps_s = stem.partition("@")
            try:
                fps = int(fps_s)
            except ValueError:
                sys.exit(f"{path.name}: bad @fps suffix")
        if len(stem.encode()) > 15:
            sys.exit(f"{path.name}: asset name '{stem}' > 15 chars")
        if stem in names:
            sys.exit(f"duplicate asset name '{stem}'")
        names[stem] = path

        data = path.read_bytes()
        entries.append(ENTRY.pack(stem.encode(), pos, len(data),
                                  KINDS[path.suffix.lower()], fps, 0))
        blobs.append(data)
        pos = (pos + len(data) + 3) & ~3

    image = HEADER.pack(MAGIC, len(files), 0) + b"".join(entries)
    image += b"\0" * ((4 - len(image) % 4) % 4)
    assert len(image) == ((HEADER.size + ENTRY.size * len(files) + 3) & ~3)
    for blob in blobs:
        image += blob
        image += b"\0" * ((4 - len(blob) % 4) % 4)

    if len(image) > args.partition_size:
        sys.exit(f"packed image {len(image)} B exceeds partition {args.partition_size} B")

    args.out.write_bytes(image)
    print(f"packed {len(files)} assets, {len(image)} B -> {args.out}")
    for (ent, path) in zip(entries, files):
        name, off, ln, kind, fps, _ = ENTRY.unpack(ent)
        print(f"  {name.rstrip(b'\\x00').decode():16s} "
              f"kind={kind} len={ln:>8} fps={fps:>2}  ({path.name})")
    print(f"flash with: esptool.py --chip esp32s3 write_image "
          f"{hex(args.offset)} {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
