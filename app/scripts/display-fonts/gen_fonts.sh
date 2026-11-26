#!/usr/bin/env bash
# Generate the tab1a design-system LVGL fonts.
#
#   ./gen_fonts.sh          # requires network + node/npx (>= 18)
#
# Downloads the OFL faces from the google/fonts repo (cached with recorded
# sha256), renders the size ramp below with lv_font_conv 1.5.3 (bpp 2, no
# compression; 1.5.3 does not take the older converter's --stride/--align),
# and writes the C files to ../src/display/fonts/. Keep the generated files
# committed so builds never need network; re-run only when the ramp or a
# face changes.
#
# Size ramp (see app/src/display/grid.md):
#   baijamjuree 144 hero clock   72 legacy hero   48 sub-hero
#               (all three: digits/punct subset)
#   dmserif      48 display serif (clock style alt)
#   inter        28 title (ASCII + Vietnamese)
#                20 body  16 caption (ASCII + UI punct: · — …)
#   barlow       24 calendar days + key tiles   20 tabular UI (stats)
#   icons        24 FontAwesome5 subset, barlow_24's fallback
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CACHE="$SCRIPT_DIR/.cache"
OUT="$SCRIPT_DIR/../../src/display/fonts"   # -> app/src/display/fonts
CONVERTER="lv_font_conv@1.5.3"
GOOGLE="https://raw.githubusercontent.com/google/fonts/main"

mkdir -p "$CACHE" "$OUT"

fetch() { # fetch <name> <repo-path>   (caches + records sha256 provenance)
    local name="$1" path="$2" dest="$CACHE/$1.ttf"
    if [[ -s "$dest" ]]; then return 0; fi
    echo "fetching $name ..."
    # URL-encode [ ] for the variable-font path
    local url="$GOOGLE/${path// /%20}"
    url="${url//[/%5B}"; url="${url//]/%5D}"
    curl -fsSL --retry 3 -o "$dest" "$url"
    shasum -a 256 "$dest" | tee "$dest.sha256"
}

# Unique symbol set for the numeric/hero faces (clock + timers + AM/PM)
HERO_SYMBOLS='0123456789:./-%+ AMP'

# Vietnamese text set (the standard block list for lv_font_conv). Faces used
# for the pomodoro quotes must carry it, or the quotes render as boxes.
VIET_RANGES='0x20-0xFF,0x102-0x103,0x110-0x111,0x1A0-0x1B0,0x300-0x309,0x31B,0x323,0x1EA0-0x1EF9'

# Non-ASCII punctuation the UI text carries; missing glyphs draw as squares.
# Needed by every face rendering TEXT_BODY/TEXT_CAPTION/TEXT_TITLE/TEXT_KEY_LABEL.
UI_PUNCT='·—…‹›'

gen() { # gen <face> <size> <-r range | --symbols syms>
    local face="$1" size="$2"; shift 2
    local out="$OUT/lv_font_tab1a_${face}_${size}.c"
    echo "generating lv_font_tab1a_${face}_${size} ..."
    npx --yes "$CONVERTER" \
        --font "$CACHE/$face.ttf" \
        --size "$size" --bpp 2 --no-compress \
        --format lvgl --lv-font-name "lv_font_tab1a_${face}_${size}" \
        --lv-include lvgl.h \
        -o "$out" "$@"
}

fetch baijamjuree "ofl/baijamjuree/BaiJamjuree-Medium.ttf"
fetch dmserif     "ofl/dmserifdisplay/DMSerifDisplay-Regular.ttf"
fetch inter       "ofl/inter/Inter[opsz,wght].ttf"
fetch barlow      "ofl/barlow/Barlow-Medium.ttf"

# OFL license texts (provenance, committed next to the fonts). Cached like
# the faces: an offline rerun must not abort before the converter runs.
mkdir -p "$OUT/LICENSES"
for face in baijamjuree:ofl/baijamjuree dmserif:ofl/dmserifdisplay inter:ofl/inter barlow:ofl/barlow; do
    name="${face%%:*}"; dir="${face##*:}"
    if [[ -s "$OUT/LICENSES/$name-OFL.txt" ]]; then continue; fi
    curl -fsSL --retry 3 -o "$OUT/LICENSES/$name-OFL.txt" "$GOOGLE/$dir/OFL.txt"
done

gen baijamjuree 144 --symbols "$HERO_SYMBOLS"
gen baijamjuree 72 --symbols "$HERO_SYMBOLS"
gen baijamjuree 48 --symbols "$HERO_SYMBOLS"
gen dmserif     48 --symbols "$HERO_SYMBOLS"
gen inter       28 -r "$VIET_RANGES"
gen inter       20 -r 0x20-0x7e --symbols "$UI_PUNCT"
gen inter       16 -r 0x20-0x7e --symbols "$UI_PUNCT"
gen barlow      24 -r 0x20-0x7e --symbols "$UI_PUNCT"
gen barlow      20 -r 0x20-0x7e --symbols "$UI_PUNCT"

# FontAwesome5 icon subset at the key-tile size: keymap_screen.c labels mix
# text with FA5 codepoints and render through barlow_24's `fallback`. Glyph
# set = pad10a icons.c (arrows f060-063, transport f04a-f04e) + volume/mute
# (f027/f028/f6a9).
ICON_FACE="fontawesome5"
ICON_RANGE="0xf027,0xf028,0xf04a,0xf04b,0xf04c,0xf04d,0xf04e,0xf060,0xf061,0xf062,0xf063,0xf0d7,0xf6a9"
if [[ ! -s "$CACHE/$ICON_FACE.woff" ]]; then
    echo "fetching fontawesome5 (LVGL built-in copy, SIL OFL) ..."
    curl -fsSL --retry 3 -o "$CACHE/$ICON_FACE.woff" \
        "https://lvgl.io/assets/others/FontAwesome5-Solid+Brands+Regular.woff"
    shasum -a 256 "$CACHE/$ICON_FACE.woff" | tee "$CACHE/$ICON_FACE.woff.sha256"
fi
echo "generating lv_font_tab1a_icons_24 ..."
npx --yes "$CONVERTER" \
    --font "$CACHE/$ICON_FACE.woff" \
    --size 24 --bpp 2 --no-compress \
    --format lvgl --lv-font-name lv_font_tab1a_icons_24 \
    --lv-include lvgl.h \
    -r "$ICON_RANGE" \
    -o "$OUT/lv_font_tab1a_icons_24.c"

# Re-apply the fallback chain: the converter always writes .fallback = NULL,
# and the keymap's icon labels break silently without this.
python3 - "$OUT/lv_font_tab1a_barlow_24.c" <<'PYEOF'
import sys, pathlib
p = pathlib.Path(sys.argv[1])
t = p.read_text()
if "lv_font_tab1a_icons_24" not in t:
    # Anchor below the conditional-include block: inside that #ifdef the
    # declare compiles away for half the builds.
    anchor = "#ifndef LV_FONT_TAB1A_BARLOW_24"
    t = t.replace(anchor,
         "/* Coban tab1a addition (a13 icon pass): the key-tile labels mix text\n"
         " * with FontAwesome5 icon codepoints; the subset font below carries the\n"
         " * icons and is chained via .fallback (keep this line + the .fallback\n"
         " * entry when regenerating via gen_fonts.sh -- the script re-applies\n"
         " * both). */\n"
         "LV_FONT_DECLARE(lv_font_tab1a_icons_24);\n\n" + anchor, 1)
t = t.replace("    .fallback = NULL,", "    .fallback = &lv_font_tab1a_icons_24,")
p.write_text(t)
print("barlow_24 fallback chained")
PYEOF

echo "done -> $OUT"
