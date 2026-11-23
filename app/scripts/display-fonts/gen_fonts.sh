#!/usr/bin/env bash
# Generate the tab1a design-system LVGL fonts (workspace backlog a07).
#
#   ./gen_fonts.sh          # requires network + node/npx (>= 18)
#
# Downloads the OFL faces from the google/fonts repo (cached with recorded
# sha256), renders the size ramp below with lv_font_conv 1.5.3 (bpp 2, no
# compression — the same rendering params pad10a used; that tree's older
# converter also passed --stride/--align, which 1.5.3 does not take), and
# writes the C files to ../src/display/fonts/ where they are committed.
# Re-run only when the ramp or a face changes; keep the generated files in
# git so builds never need network.
#
# Size ramp (see app/src/display/grid.md):
#   baijamjuree 144 hero clock (wave-1: doubled)  72 legacy hero  48 sub-hero
#                (all three: digits/punct subset)
#   dmserif      48 display serif   (clock style alt, a09)
#   inter        28 title  (ASCII + Vietnamese: a10 quote screen)
#                20 body  16 caption    (ASCII + UI punct: · — …)
#   barlow       24 calendar days  20 tabular UI alt (stats, a11)
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

# Vietnamese text set (the standard block list for lv_font_conv): Latin-1
# precomposed accents + breve/horn/dhook + Latin Extended Additional
# (hook/dot below). Needed by the pomodoro quotes (a10, pad10a
# screen_pomodoro.c:414-463); faces carrying it must stay >0 px Vietnamese
# or quotes render as boxes.
VIET_RANGES='0x20-0xFF,0x102-0x103,0x110-0x111,0x1A0-0x1B0,0x300-0x309,0x31B,0x323,0x1EA0-0x1EF9'

# Punctuation the UI text actually carries beyond ASCII (owner report
# 2026-11-23: missing glyphs drew as squares): middle-dot separators in
# the pomo dialog facts, the keymap em dash, the debug ellipsis. Needed
# by every face that renders TEXT_BODY/TEXT_CAPTION/TEXT_TITLE.
UI_PUNCT='·—…'

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
gen barlow      24 -r 0x20-0x7e
gen barlow      20 -r 0x20-0x7e

echo "done -> $OUT"
