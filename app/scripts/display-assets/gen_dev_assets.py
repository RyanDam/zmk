#!/usr/bin/env python3
"""Generate the tab1a dev assets (workspace backlog a12) into the
zmk-config assets directory, themed to match the design-system defaults
(CONFIG_ZMK_DISPLAY_THEME_*). Self-made art: no licensing debt, and the
real product art will replace these via a15 uploads.

    ./gen_dev_assets.py <out-dir>

Requires Pillow (present in zmk/.venv). Run only when the dev set
changes; pack with pack_assets.py next to it.
"""

import json
import math
import sys
from pathlib import Path

from PIL import Image, ImageDraw

BG = (0x14, 0x16, 0x1C)
ACCENT = (0xE8, 0xA3, 0x3D)
INK = (0x3A, 0x3F, 0x49)


def boot_gif(out: Path) -> None:
    """160x160, 8 frames: a brush ring draws itself, dot pops in."""
    size, frames, c, r = 160, 8, (80, 80), 52
    imgs = []
    for i in range(frames):
        img = Image.new("P", (size, size))
        img.putpalette([v for c2 in (BG, ACCENT, INK) for v in c2] + [0] * (768 - 9))
        d = ImageDraw.Draw(img)
        d.rectangle([0, 0, size, size], fill=0)
        # ring arc grows around the circle (240 steps of arc per frame)
        end = -90 + 360.0 * (i + 1) / frames
        if i < frames - 1:
            d.arc([c[0] - r, c[1] - r, c[0] + r, c[1] + r], -90, end, fill=1, width=6)
        else:
            d.ellipse([c[0] - r, c[1] - r, c[0] + r, c[1] + r], outline=1, width=6)
            dr = 10 + (i * 3) % 6
            d.ellipse([c[0] - dr, c[1] - dr, c[0] + dr, c[1] + dr], fill=1)
        # a faint ink tick marks every quarter (stationery detail)
        for q in range(4):
            if 90 * q <= (360.0 * (i + 1) / frames):
                a = math.radians(-90 + 90 * q)
                x1, y1 = c[0] + (r - 12) * math.cos(a), c[1] + (r - 12) * math.sin(a)
                x2, y2 = c[0] + (r - 4) * math.cos(a), c[1] + (r - 4) * math.sin(a)
                d.line([x1, y1, x2, y2], fill=2, width=3)
        imgs.append(img)
    imgs[0].save(out, save_all=True, append_images=imgs[1:], duration=120, loop=0)


def animbg_gif(out: Path) -> None:
    """320x480, 6 frames @ ~4 fps: faint diagonal paper grain drifting,
    one warm lamp-glow arc low on the panel (the stationery-desk mood)."""
    w, h, frames = 320, 480, 6
    imgs = []
    for i in range(frames):
        img = Image.new("P", (w, h))
        img.putpalette([v for c2 in (BG, (0x1A, 0x1D, 0x24), (0x24, 0x27, 0x30),
                                      (0x2B, 0x25, 0x1B)) for v in c2] + [0] * (768 - 12))
        d = ImageDraw.Draw(img)
        d.rectangle([0, 0, w, h], fill=0)
        drift = i * 8
        for y in range(-40 + drift % 16, h, 16):  # slow paper grain bands
            d.line([0, y, w, y - 14], fill=1, width=1)
        for y in range(-40 + (drift * 2) % 32, h, 48):  # heavier fold every 3rd
            d.line([0, y, w, y - 20], fill=2, width=1)
        # lamp arc near the bottom, breathing 2px
        r = 210 + (2 if i in (2, 3) else 0)
        d.arc([w // 2 - r, h - r, w // 2 + r, h + r], 180, 360, fill=3, width=10)
        imgs.append(img)
    imgs[0].save(out, save_all=True, append_images=imgs[1:], duration=250, loop=0)


def boot_lottie(out: Path) -> None:
    """Minimal hand-written Lottie: amber dot pulsing (scale 60->100->60).
    Kept deliberately simple (one shape layer) — ThorVG parses the common
    subset; fancy features are not guaranteed."""
    def key(t, s):
        return {"t": t, "s": [s, s], "to": [0, 0], "ti": [0, 0]}

    anim = {
        "v": "5.7.4", "fr": 30, "ip": 0, "op": 60, "w": 120, "h": 120, "nm": "pulse",
        "layers": [{
            "ty": 4, "nm": "dot", "ip": 0, "op": 60, "st": 0,
            "ks": {
                "o": {"a": 0, "k": 100},
                "p": {"a": 0, "k": [60, 60, 0]},
                "a": {"a": 0, "k": [0, 0, 0]},
                "s": {"a": 1, "k": [key(0, 60), key(30, 100), key(60, 60)]},
                "r": {"a": 0, "k": 0},
            },
            "shapes": [{
                "ty": "gr",
                "it": [
                    {"ty": "el", "p": {"a": 0, "k": [0, 0]}, "s": {"a": 0, "k": [80, 80]}},
                    {"ty": "fl", "c": {"a": 0, "k": [0.910, 0.639, 0.239, 1]}, "o": {"a": 0, "k": 100}},
                    {"ty": "tr", "p": {"a": 0, "k": [0, 0]}, "a": {"a": 0, "k": [0, 0]},
                     "s": {"a": 0, "k": [100, 100]}, "r": {"a": 0, "k": 0}, "o": {"a": 0, "k": 100}},
                ],
            }],
        }],
    }
    out.write_text(json.dumps(anim), encoding="utf-8")


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)

    boot_gif(out / "boot@8.gif")
    boot_lottie(out / "pulse.json")
    animbg_gif(out / "animbg@4.gif")
    for p in sorted(out.iterdir()):
        print(f"  {p.name}: {p.stat().st_size} B")
    return 0


if __name__ == "__main__":
    sys.exit(main())
