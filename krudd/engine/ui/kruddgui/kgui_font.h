/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef KGUI_FONT_H
#define KGUI_FONT_H

#include <stdint.h>

#include "kgui_batch.h" /* struct kgui_glyph, kgui_glyph_fn */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * kgui_font — kruddgui's own baked glyph atlas, GL-free and host-testable.
 *
 * Until now kruddgui borrowed Dear ImGui's font atlas to draw text (see the
 * old glyph_source in kruddgui.cpp): a code point outside that atlas rendered
 * as ImGui's '?', and text could not exist without ImGui at all. This module
 * removes that coupling — it bakes an embedded bitmap font into an RGBA atlas
 * in plain memory and answers the same kgui_glyph_fn queries kgui_batch already
 * speaks, so kruddgui.cpp only has to upload the pixels to a texture. The bake
 * is pure computation (no GL, no ImGui), so the atlas layout and every glyph's
 * metrics are checked on the host in kgui_font_test.c.
 *
 * The embedded face is the public-domain 8x8 bitmap font (font8x8, basic
 * latin, U+0020..U+007F). Glyphs are trimmed to their ink width so the font
 * reads as proportional rather than fixed-pitch; the full 8-row cell height is
 * kept for a consistent baseline. Non-ASCII code points have no glyph and are
 * skipped by the caller (kgui_batch_text), the honest v0 of an owned pipeline;
 * a scalable/antialiased face is a later refinement.
 *
 * The atlas is straight (non-premultiplied) RGBA: every texel is white with
 * coverage in the alpha channel, and one reserved texel is fully opaque white
 * for kruddgui's filled rectangles. That lets the single kruddgui shader (colour
 * times sampled texel) draw both glyphs and solid rects without a branch.
 */

#define KGUI_FONT_FIRST 0x20 /* first code point baked (space)            */
#define KGUI_FONT_LAST  0x7F /* last code point baked (DEL, blank)        */
#define KGUI_FONT_COUNT (KGUI_FONT_LAST - KGUI_FONT_FIRST + 1) /* 96 */
#define KGUI_FONT_CELL  8    /* native glyph cell, px (font8x8 is 8x8)    */
#define KGUI_FONT_COLS  16   /* glyph cells per atlas row                 */
#define KGUI_FONT_ROWS  ((KGUI_FONT_COUNT + KGUI_FONT_COLS - 1) / KGUI_FONT_COLS)

/*
 * Atlas dimensions. The glyph grid is KGUI_FONT_ROWS rows of cells; one extra
 * cell-high strip below it holds the solid white texel for filled rects.
 */
#define KGUI_FONT_ATLAS_W (KGUI_FONT_COLS * KGUI_FONT_CELL)
#define KGUI_FONT_ATLAS_H ((KGUI_FONT_ROWS + 1) * KGUI_FONT_CELL)

/*
 * The default text size kruddgui draws at, in CSS pixels. A whole multiple of
 * the 8px cell keeps the nearest-sampled bitmap crisp; the panels read this
 * back through (kgui-text-metrics) and lay out against it, so changing it here
 * re-flows the UI rather than needing per-call sizes.
 */
#define KGUI_FONT_SIZE 16.0f

/*
 * One glyph's baked metrics. Columns are the inclusive ink span within the 8px
 * cell (c1 < c0 marks a blank glyph such as space); advance is the pen step in
 * native px. Kept as bytes because every value fits: a span is 0..7 and an
 * advance is at most cell width plus one.
 */
struct kgui_font_metric {
	uint8_t c0, c1;   /* ink column span, inclusive (c1 < c0 => blank) */
	uint8_t advance;  /* pen advance in native px                      */
	uint8_t visible;  /* 0 for a blank glyph (emits no quad)           */
};

/*
 * A baked font: the RGBA atlas, the per-glyph metrics, the white-pixel UV for
 * filled rects, and the default draw size. Caller-owned (kruddgui keeps one
 * static instance); kgui_font_init fills it from the embedded face.
 */
struct kgui_font {
	unsigned char pixels[KGUI_FONT_ATLAS_W * KGUI_FONT_ATLAS_H * 4];
	struct kgui_font_metric metrics[KGUI_FONT_COUNT];
	float white_u, white_v; /* UV of the solid opaque-white texel */
	float size;             /* default draw size, CSS px          */
};

/* Bake the embedded face into `f` (atlas pixels + metrics + white pixel). */
void kgui_font_init(struct kgui_font *f);

/*
 * Resolve one code point at a pixel size into a glyph box and atlas UVs. Signed
 * to match kgui_glyph_fn: `ud` is a struct kgui_font *. Returns 0 for a code
 * point the face does not carry (the caller skips it), 1 otherwise — a blank
 * glyph returns 1 with visible == 0 and a non-zero advance.
 */
int kgui_font_glyph(void *ud, uint32_t cp, float size,
		    struct kgui_glyph *out);

#ifdef __cplusplus
}
#endif

#endif /* KGUI_FONT_H */
