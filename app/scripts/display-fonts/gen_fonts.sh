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
#   baijamjuree  72 hero clock      48 sub-hero      (digits/punct subset)
#   dmserif      48 display serif   (clock style alt, a09)
#   inter        28 title  20 body  16 caption       (full ASCII)
#   barlow       20 tabular UI alt  (stats, a11)
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

# OFL license texts (provenance, committed next to the fonts)
mkdir -p "$OUT/LICENSES"
for face in baijamjuree:ofl/baijamjuree dmserif:ofl/dmserifdisplay inter:ofl/inter barlow:ofl/barlow; do
    name="${face%%:*}"; dir="${face##*:}"
    curl -fsSL --retry 3 -o "$OUT/LICENSES/$name-OFL.txt" "$GOOGLE/$dir/OFL.txt"
done

gen baijamjuree 72 --symbols "$HERO_SYMBOLS"
gen baijamjuree 48 --symbols "$HERO_SYMBOLS"
gen dmserif     48 --symbols "$HERO_SYMBOLS"
gen inter       28 -r 0x20-0x7e
gen inter       20 -r 0x20-0x7e
gen inter       16 -r 0x20-0x7e
gen barlow      20 -r 0x20-0x7e

echo "done -> $OUT"
