<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# kruddgui font asset

kruddgui bakes its own glyph atlas at build time from the TrueType font here
(see `../kgui_font.c`). The build embeds `ui_font.ttf` byte-exact into the WASM
module as `UI_FONT_TTF` / `UI_FONT_TTF_LEN` (via `krudd-embed-binary-optional`
in `kruddmake/ninja.scm`), and `kruddgui.cpp` bakes it once at startup.

## Vendored font: Inter

`ui_font.ttf` is **Inter** (`InterVariable.ttf`, version 4.001), by the Inter
Project Authors, under the SIL Open Font License 1.1 — see `OFL.txt` and the
`third_party/VENDOR.md` entry. It came from the author's official site
(https://rsms.me/inter/) unmodified, so the OFL's Reserved-Font-Name /
modification clauses do not apply — we embed it as-is and rasterise at runtime.

Two things worth knowing about baking Inter through stb_truetype:

- **Kerning is inert with this font.** `kgui_font` reads pair kerning from the
  legacy `kern` table (`stbtt_GetCodepointKernAdvance`); Inter ships kerning
  only as GPOS pairs, which stb_truetype does not read, so every pair returns a
  zero adjustment. The kern *seam* is in place and correct — a font with a
  legacy `kern` table lights it up immediately — but live pair kerning for Inter
  itself waits on GPOS shaping (the same future work as complex scripts).
- **It's a variable font.** stb_truetype ignores the variation axes and bakes
  the default master (Regular). That is what we want here; there is no static
  Regular on the upstream site.

Inter is the full multi-script face (~860 KB), of which only Latin is baked —
the rest is dead weight in the module. Subsetting it to the baked ranges (e.g.
`pyftsubset`) would shrink the embed substantially and is a clean follow-up.

## Swapping the font

Replace `ui_font.ttf` (keep the name) and update `OFL.txt` + the VENDOR.md
entry. Requirements:

- **Format:** a single TrueType (`.ttf`) face; `kgui_font` bakes face index 0.
- **Coverage:** at minimum Basic Latin (U+0020–U+007E); Latin-1 Supplement
  (U+00A0–U+00FF) is baked too when present. These are the ranges in
  `k_ranges[]` in `kgui_font.c` — widen both together for more.
- **License:** must be redistributable under a license compatible with the
  repository; record it here and in `third_party/VENDOR.md`.

## Building without a font

The embed is *optional*: with no `ui_font.ttf` present the build still succeeds,
emitting a zero-length `UI_FONT_TTF`. `kgui_font_bake` treats that as "no font"
and kruddgui draws no text (logging once), so panels still lay out and respond —
only their labels are blank until a font is present.
