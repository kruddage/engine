<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# kruddgui font asset

kruddgui bakes its own glyph atlas at build time from a TrueType font embedded
here (see `../kgui_font.c`). Drop the UI font at:

```
ui_font.ttf
```

The build embeds it byte-exact into the WASM module as `UI_FONT_TTF` /
`UI_FONT_TTF_LEN` (via `krudd-embed-binary-optional` in `kruddmake/ninja.scm`),
and `kruddgui.cpp` bakes it once at startup.

## Requirements

- **Format:** a single TrueType (`.ttf`) face. `kgui_font` bakes face index 0.
- **Coverage:** at minimum Basic Latin (U+0020–U+007E); Latin-1 Supplement
  (U+00A0–U+00FF) is baked too when present. These are the ranges in
  `k_ranges[]` in `kgui_font.c` — widen both together if you need more.
- **Kerning:** a legacy `kern` table gets picked up automatically
  (`stbtt_GetCodepointKernAdvance`). Fonts that ship kerning only as GPOS pairs
  will bake and render but without pair kerning.
- **License:** must be redistributable under a license compatible with the
  repository. Record it alongside the file (e.g. an `OFL.txt`) and note it in
  `third_party/VENDOR.md` if it is third-party.

## Until the font is added

The embed is *optional*: with no `ui_font.ttf` present the build still succeeds,
emitting a zero-length `UI_FONT_TTF`. `kgui_font_bake` treats that as "no font"
and kruddgui draws no text (logging once), so panels still lay out and respond —
only their labels are blank until the font lands.
