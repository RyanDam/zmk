# tab1a display fonts — generated, do not hand edit

Every `lv_font_tab1a_*.c` here is output of
[`../../../scripts/display-fonts/gen_fonts.sh`](../../../scripts/display-fonts/gen_fonts.sh)
(lv_font_conv 1.5.3) against the design-system spec in
[`../styles.h`](../styles.h) / [`../theme.c`](../theme.c). Regenerate instead of
patching a glyph table:

```sh
sh app/scripts/display-fonts/gen_fonts.sh     # sources cached in app/scripts/display-fonts/.cache
```

The generator records its exact arguments in the first comment block of each
file (`Opts:`), so a file here is reproducible from that line alone; a diff that
changes only glyph arrays with an unchanged `Opts:` line means someone edited
generated output — regenerate instead.

`LICENSES/` keeps each face's licence (SIL OFL text), required because the
rasters embed the typefaces. One known gap: `lv_font_tab1a_icons_24.c` subsets
FontAwesome 5 Free (fetched from LVGL's built-in copy,
[`gen_fonts.sh:106`](../../../scripts/display-fonts/gen_fonts.sh), also OFL) and has
no `LICENSES/` entry yet — OFL attribution means it should get one.

`.gitattributes` marks this directory `linguist-generated -diff`: ~17k lines of
glyph tables would otherwise dominate every review.
