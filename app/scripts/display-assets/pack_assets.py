#!/usr/bin/env python3
"""Pack a directory of dev art into the Coban Tab 1a asset store image
(workspace backlog a12; v2 generation format since a15).

    ./pack_assets.py <assets-dir> <out.bin> [--partition-size 0x1F8000]

The image layout must match app/src/assets.c ("keep the two in sync"):

    header : b"ZMKAS002" | entry_count u32le | generation u32le     (16 B)
    entry  : name[16] | offset u32 | len u32 | kind u16 | fps u16
             | crc32 u32                                             (32 B)
    crc    : crc32(header || entries) u32le
    blobs  : file data, 4-byte aligned, from sector 1 (offset 4096)

v1 (`ZMKAS001`, a12) stays bootable: the firmware probes it read-only
and the first host upload GC-compacts onto this v2 layout. This script
always writes v2, so dev images and firmware commits share one shape.

CRCs are zlib CRC-32 (== firmware crc32_ieee): per-blob in the entry,
and a manifest trailer checked before any generation is trusted — the
same integrity the a15 upload path verifies against.

Asset name = filename without extension; an optional "@fps" suffix
("meadow@12.gif") records the authored frame rate (the firmware warns
above 15 fps, the panel's ~16 fps full-refresh ceiling). Extensions:
.gif -> kind 0, .json -> kind 1 (Lottie). Flash the result at the
`assets` partition offset (0xda8000 on cobantab1a, see the board DTS).
"""

import argparse
import struct
import sys
import zlib
from pathlib import Path

MAGIC = b"ZMKAS002"
HEADER = struct.Struct("<8sII")
ENTRY = struct.Struct("<16sIIHHI")
DATA_FLOOR = 4096  # blobs start at sector 1 so a manifest rewrite never
                   # shares a sector with art (matches assets.c)
KINDS = {".gif": 0, ".json": 1}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("assets_dir", type=Path)
    ap.add_argument("out", type=Path)
    ap.add_argument("--partition-size", type=lambda s: int(s, 0), default=0x1F8000,
                    help="assets partition size (2016 KiB since the a15 carve fix)")
    ap.add_argument("--offset", type=lambda s: int(s, 0), default=0xDA8000,
                    help="partition offset, for the printed flash command")
    ap.add_argument("--generation", type=int, default=1,
                    help="manifest generation stamp (host images own gen 1)")
    args = ap.parse_args()

    files = sorted(p for p in args.assets_dir.iterdir()
                   if p.is_file() and p.suffix.lower() in KINDS)
    if not files:
        print(f"no .gif/.json assets in {args.assets_dir}", file=sys.stderr)

    names = {}
    entries, blobs = [], []
    pos = DATA_FLOOR  # every blob lands sector-aligned above the manifest

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
                                  KINDS[path.suffix.lower()], fps,
                                  zlib.crc32(data) & 0xFFFFFFFF))
        blobs.append(data)
        pos = (pos + len(data) + 3) & ~3

    head = HEADER.pack(MAGIC, len(files), args.generation)
    body = b"".join(entries)
    image = head + body + struct.pack("<I", zlib.crc32(head + body) & 0xFFFFFFFF)
    image += b"\xff" * (DATA_FLOOR - len(image))  # erased-looking gap
    for blob in blobs:
        image += blob
        image += b"\0" * ((4 - len(blob) % 4) % 4)

    if len(image) > args.partition_size:
        sys.exit(f"packed image {len(image)} B exceeds partition {args.partition_size} B")

    args.out.write_bytes(image)
    print(f"packed {len(files)} assets, {len(image)} B -> {args.out} (gen {args.generation})")
    for (ent, path) in zip(entries, files):
        name, off, ln, kind, fps, crc = ENTRY.unpack(ent)
        label = name.rstrip(b"\0").decode()
        print(f"  {label:16s} "
              f"kind={kind} len={ln:>8} fps={fps:>2} crc={crc:08x}  ({path.name})")
    print(f"flash with: esptool.py --chip esp32s3 write_image "
          f"{hex(args.offset)} {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
