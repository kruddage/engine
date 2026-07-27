/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * kgui_font — see kgui_font.h. Bakes the embedded JetBrains Mono face into an
 * SDF RGBA atlas with stb_truetype and answers kgui_batch's glyph queries from
 * it. No GL, no ImGui — pure computation, so kgui_font_test exercises the whole
 * bake on the host.
 */

#include "kgui_font.h"

#include <string.h>

/*
 * The vendored rasteriser. STBTT_STATIC keeps every stb symbol file-local (no
 * link surface leaks into kruddgui or the tests); the implementation lives in
 * this one translation unit. That makes the stb entry points we do not call
 * (the packing/name-table API) unused statics, which the engine's -Werror build
 * would reject — so the upstream header is included under a diagnostic guard, as
 * third-party code the engine's warning flags do not police.
 */
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_truetype.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include "jetbrains_mono.h" /* k_jetbrains_mono_ttf[], _LEN */

/* Write one atlas texel: white RGB, `a` in alpha (the SDF distance). */
static void put_texel(struct kgui_font *f, int x, int y, unsigned char a)
{
	unsigned char *px = &f->pixels[(y * KGUI_FONT_ATLAS_W + x) * 4];

	px[0] = 255;
	px[1] = 255;
	px[2] = 255;
	px[3] = a;
}

/*
 * Copy one glyph's stb SDF bitmap into its atlas cell (alpha channel) and record
 * its metrics. `sdf` may be NULL (a blank glyph such as space): then only the
 * advance is stored and the glyph emits no quad. The copy is clipped to the cell
 * so an unexpectedly large bitmap can never write out of bounds.
 */
static void place_glyph(struct kgui_font *f, int idx, int cell_x, int cell_y,
			const unsigned char *sdf, int w, int h,
			int xoff, int yoff, int advance)
{
	struct kgui_font_metric *m = &f->metrics[idx];
	int r, c;

	if (!sdf || w <= 0 || h <= 0) {
		m->ax = m->ay = 0;
		m->w = m->h = 0;
		m->xoff = m->yoff = 0;
		m->advance = (uint8_t)advance;
		m->visible = 0;
		return;
	}

	if (w > KGUI_FONT_CELL_W)
		w = KGUI_FONT_CELL_W;
	if (h > KGUI_FONT_CELL_H)
		h = KGUI_FONT_CELL_H;

	for (r = 0; r < h; r++)
		for (c = 0; c < w; c++)
			put_texel(f, cell_x + c, cell_y + r,
				  sdf[r * w + c]);

	m->ax      = (uint16_t)cell_x;
	m->ay      = (uint16_t)cell_y;
	m->w       = (uint8_t)w;
	m->h       = (uint8_t)h;
	m->xoff    = (int8_t)xoff;
	m->yoff    = (int8_t)yoff;
	m->advance = (uint8_t)advance;
	m->visible = 1;
}

void kgui_font_init(struct kgui_font *f)
{
	stbtt_fontinfo info;
	float scale;
	float dist_scale = (float)KGUI_FONT_SDF_ONEDGE / (float)KGUI_FONT_SDF_PAD;
	int white_y = KGUI_FONT_ROWS * KGUI_FONT_CELL_H;
	int ascent, descent, line_gap;
	int x, y, i;

	/* Whole atlas: white RGB, alpha 0 (fully "outside" the field). */
	for (y = 0; y < KGUI_FONT_ATLAS_H; y++)
		for (x = 0; x < KGUI_FONT_ATLAS_W; x++)
			put_texel(f, x, y, 0);

	stbtt_InitFont(&info, k_jetbrains_mono_ttf,
		       stbtt_GetFontOffsetForIndex(k_jetbrains_mono_ttf, 0));
	scale = stbtt_ScaleForPixelHeight(&info, (float)KGUI_FONT_BAKE_PX);
	stbtt_GetFontVMetrics(&info, &ascent, &descent, &line_gap);
	f->ascent = (float)ascent * scale;

	for (i = 0; i < KGUI_FONT_COUNT; i++) {
		int cp = KGUI_FONT_FIRST + i;
		int cell_x = (i % KGUI_FONT_COLS) * KGUI_FONT_CELL_W;
		int cell_y = (i / KGUI_FONT_COLS) * KGUI_FONT_CELL_H;
		int w = 0, h = 0, xoff = 0, yoff = 0, adv = 0, lsb;
		unsigned char *sdf;

		stbtt_GetCodepointHMetrics(&info, cp, &adv, &lsb);
		adv = (int)((float)adv * scale + 0.5f);

		sdf = stbtt_GetCodepointSDF(&info, scale, cp, KGUI_FONT_SDF_PAD,
					    KGUI_FONT_SDF_ONEDGE, dist_scale,
					    &w, &h, &xoff, &yoff);
		place_glyph(f, i, cell_x, cell_y, sdf, w, h, xoff, yoff, adv);
		stbtt_FreeSDF(sdf, NULL);
	}

	/*
	 * Solid "inside" strip below the glyphs: alpha 255 reads as deep inside
	 * the field, so a filled rect point-sampling it resolves to full coverage
	 * through the same SDF shader the glyphs use.
	 */
	for (y = white_y; y < KGUI_FONT_ATLAS_H; y++)
		for (x = 0; x < KGUI_FONT_ATLAS_W; x++)
			put_texel(f, x, y, 255);

	/* Sample the centre of a texel well inside the solid strip. */
	f->white_u = (2.0f + 0.5f) / (float)KGUI_FONT_ATLAS_W;
	f->white_v = ((float)white_y + 2.0f + 0.5f) / (float)KGUI_FONT_ATLAS_H;
	f->size    = KGUI_FONT_SIZE;
}

int kgui_font_glyph(void *ud, uint32_t cp, float size, struct kgui_glyph *out)
{
	struct kgui_font              *f = (struct kgui_font *)ud;
	const struct kgui_font_metric *m;
	float disp;

	if (!f || cp < KGUI_FONT_FIRST || cp > KGUI_FONT_LAST)
		return 0;

	m    = &f->metrics[cp - KGUI_FONT_FIRST];
	disp = size / (float)KGUI_FONT_BAKE_PX;

	if (!m->visible) {
		out->x0 = out->y0 = out->x1 = out->y1 = 0.0f;
		out->u0 = out->v0 = out->u1 = out->v1 = 0.0f;
		out->advance = (float)m->advance * disp;
		out->visible = 0;
		return 1;
	}

	/*
	 * Box relative to the pen origin (a line's top-left). The baseline drops
	 * f->ascent from the top; the glyph's bitmap sits (xoff, yoff) from the
	 * pen on the baseline, so its top is (ascent + yoff) below the line top.
	 */
	out->x0 = (float)m->xoff * disp;
	out->y0 = (f->ascent + (float)m->yoff) * disp;
	out->x1 = ((float)m->xoff + (float)m->w) * disp;
	out->y1 = (f->ascent + (float)m->yoff + (float)m->h) * disp;

	out->u0 = (float)m->ax / (float)KGUI_FONT_ATLAS_W;
	out->u1 = (float)(m->ax + m->w) / (float)KGUI_FONT_ATLAS_W;
	out->v0 = (float)m->ay / (float)KGUI_FONT_ATLAS_H;
	out->v1 = (float)(m->ay + m->h) / (float)KGUI_FONT_ATLAS_H;

	out->advance = (float)m->advance * disp;
	out->visible = 1;
	return 1;
}
