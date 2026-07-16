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
 * kruddgui draws its own text: this module bakes an embedded TrueType face into
 * a signed-distance-field (SDF) atlas in plain memory and answers the same
 * kgui_glyph_fn queries kgui_batch already speaks, so kruddgui.cpp only has to
 * upload the pixels to a texture. The bake is pure computation (no GL, no
 * ImGui), so the atlas layout and every glyph's metrics are checked on the host
 * in kgui_font_test.c.
 *
 * The embedded face is JetBrains Mono Regular, subset to printable ASCII
 * (U+0020..U+007E) — see jetbrains_mono.h. It is rasterised with the vendored
 * stb_truetype (../../third_party/stb_truetype.h) into per-glyph SDF bitmaps
 * that are packed into a fixed grid. Non-ASCII code points have no glyph and are
 * skipped by the caller (kgui_batch_text). Being monospace the face has no
 * kerning to apply; every glyph shares one advance.
 *
 * Why SDF rather than a straight coverage bitmap: a distance field stays crisp
 * when the display size differs from the bake size (headings scale up, hi-DPI
 * scales the whole UI), and the fragment shader resolves the edge per-pixel with
 * a screen-space smoothstep — antialiased text at any size from one atlas.
 *
 * The atlas is straight (non-premultiplied) RGBA. Colour comes from the vertex,
 * so every texel is white in RGB (r=g=b=255) and the ALPHA channel carries the
 * signed distance: KGUI_FONT_SDF_ONEDGE (128) is the glyph edge, larger is
 * inside, smaller is outside. One reserved texel is baked fully "inside"
 * (alpha=255) so kruddgui's filled rectangles, which point-sample it, resolve to
 * solid through the very same SDF shader path — no branch between glyphs and
 * rects. (Image quads, which sample an external RGBA texture, are the one case
 * the shader treats as a straight passthrough; see kruddgui.cpp.)
 */

#define KGUI_FONT_FIRST 0x20 /* first code point baked (space)            */
#define KGUI_FONT_LAST  0x7E /* last code point baked (~)                 */
#define KGUI_FONT_COUNT (KGUI_FONT_LAST - KGUI_FONT_FIRST + 1) /* 95 */

/*
 * Bake parameters. Glyphs are rasterised at KGUI_FONT_BAKE_PX pixels tall with
 * KGUI_FONT_SDF_PAD texels of distance spread around the ink; the spread is the
 * room the edge has to ramp, so it bounds how far the field stays valid when the
 * glyph is scaled up. KGUI_FONT_SDF_ONEDGE is the alpha value that marks the
 * outline (the shader thresholds at 0.5 == 128/255).
 */
#define KGUI_FONT_BAKE_PX   32   /* glyph raster height, px                 */
#define KGUI_FONT_SDF_PAD   4    /* distance-field spread, texels           */
#define KGUI_FONT_SDF_ONEDGE 128 /* alpha value at the glyph edge           */

/*
 * Atlas grid. A cell holds one glyph's padded SDF bitmap; the cell is sized once
 * (generously) to fit any ASCII glyph at the bake size, and the bake asserts
 * every glyph fits. One extra cell-high strip below the grid holds the solid
 * "inside" texel for filled rects.
 */
#define KGUI_FONT_COLS   16  /* glyph cells per atlas row                  */
#define KGUI_FONT_ROWS   ((KGUI_FONT_COUNT + KGUI_FONT_COLS - 1) / KGUI_FONT_COLS)
#define KGUI_FONT_CELL_W 32  /* cell width, texels  (fits advance + pad)   */
#define KGUI_FONT_CELL_H 48  /* cell height, texels (fits em + pad)        */

#define KGUI_FONT_ATLAS_W (KGUI_FONT_COLS * KGUI_FONT_CELL_W)
#define KGUI_FONT_ATLAS_H ((KGUI_FONT_ROWS + 1) * KGUI_FONT_CELL_H)

/*
 * The default text size kruddgui draws at, in CSS pixels. The panels read this
 * back through (kgui-text-metrics) and lay out against it, so changing it here
 * re-flows the UI rather than needing per-call sizes. It is independent of the
 * bake size: the SDF is sampled and smoothstepped to whatever size is asked for.
 */
#define KGUI_FONT_SIZE 16.0f

/*
 * One glyph's baked metrics, in bake-pixel units (kgui_font_glyph scales them to
 * the requested size). The glyph's SDF bitmap sits at (ax, ay) in the atlas and
 * is w x h texels; (xoff, yoff) place its top-left relative to the pen origin on
 * the baseline (y down, so yoff is negative for the part above the baseline).
 */
struct kgui_font_metric {
	uint16_t ax, ay;   /* atlas texel of the bitmap top-left */
	uint8_t  w, h;     /* SDF bitmap size, texels            */
	int8_t   xoff;     /* bitmap left, relative to pen        */
	int8_t   yoff;     /* bitmap top, relative to baseline    */
	uint8_t  advance;  /* pen advance, bake px                */
	uint8_t  visible;  /* 0 for a blank glyph (emits no quad) */
};

/*
 * A baked font: the RGBA atlas, the per-glyph metrics, the ascent (bake px, the
 * baseline's drop from a line's top), the "inside" texel UV for filled rects,
 * and the default draw size. Caller-owned (kruddgui keeps one static instance);
 * kgui_font_init fills it from the embedded face.
 */
struct kgui_font {
	unsigned char pixels[KGUI_FONT_ATLAS_W * KGUI_FONT_ATLAS_H * 4];
	struct kgui_font_metric metrics[KGUI_FONT_COUNT];
	float ascent;           /* baseline drop from line top, bake px */
	float white_u, white_v; /* UV of the solid "inside" texel       */
	float size;             /* default draw size, CSS px            */
};

/* Bake the embedded face into `f` (atlas pixels + metrics + inside texel). */
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
