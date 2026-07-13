/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * kgui_font — see kgui_font.h. Bakes an R8 glyph atlas from a TrueType blob
 * with stb_truetype and answers kgui_batch's glyph + kern seams. GL-free: it
 * only produces the atlas bitmap in CPU memory; kruddgui.cpp uploads it.
 */

#include "kgui_font.h"

#include <stdlib.h>
#include <string.h>

/*
 * stb_truetype is vendored, public-domain third-party code; like s7 it is not
 * held to the engine's -Werror -Wpedantic. Its *implementation* lives in this
 * one translation unit (STB_TRUETYPE_IMPLEMENTATION), kept static to this file
 * (STBTT_STATIC), with the compiler's diagnostics quieted around the include so
 * our own code above and below still compiles warnings-as-errors.
 */
#if defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wpedantic"
#  pragma clang diagnostic ignored "-Wcast-qual"
#  pragma clang diagnostic ignored "-Wsign-conversion"
#  pragma clang diagnostic ignored "-Wconversion"
#  pragma clang diagnostic ignored "-Wunused-function"
#  pragma clang diagnostic ignored "-Wdouble-promotion"
#elif defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wpedantic"
#  pragma GCC diagnostic ignored "-Wcast-qual"
#  pragma GCC diagnostic ignored "-Wconversion"
#  pragma GCC diagnostic ignored "-Wunused-function"
#  pragma GCC diagnostic ignored "-Wdouble-promotion"
#endif

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

#if defined(__clang__)
#  pragma clang diagnostic pop
#elif defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif

/*
 * The code-point ranges baked into the atlas. Contiguous runs, scanned by the
 * glyph source. Basic Latin printable (0x20..0x7E) plus the Latin-1 Supplement
 * (0xA0..0xFF) — enough for the mode-bar labels and Western European text; wider
 * coverage (a dynamic CJK atlas, shaping) is future work layered on this seam.
 */
static const struct {
	uint32_t first;
	int      count;
} k_ranges[] = {
	{ 0x20u, 0x7Fu - 0x20u }, /* 0x20..0x7E — 95 glyphs   */
	{ 0xA0u, 0x100u - 0xA0u }, /* 0xA0..0xFF — 96 glyphs  */
};
#define K_RANGE_COUNT ((int)(sizeof(k_ranges) / sizeof(k_ranges[0])))

static void free_ranges(struct kgui_font *f)
{
	int i;

	for (i = 0; i < f->range_count; i++) {
		free(f->ranges[i].packed);
		f->ranges[i].packed = NULL;
	}
	f->range_count = 0;
}

void kgui_font_free(struct kgui_font *f)
{
	if (!f)
		return;
	free_ranges(f);
	free(f->pixels);
	free(f->info);
	memset(f, 0, sizeof(*f));
}

/*
 * Stamp a 2×2 solid-white block into the bottom-right corner of the atlas and
 * point (white_u, white_v) at its centre, so kgui-rect's textured fills sample
 * opaque white even under bilinear filtering. The corner is outside stb's
 * top-left packing for an atlas this size; if a glyph ever reached it the only
 * cost is two corner texels, which no baked range occupies here.
 */
static void reserve_white_texel(struct kgui_font *f)
{
	int w = f->atlas_w;
	int h = f->atlas_h;

	f->pixels[(size_t)(h - 1) * w + (w - 1)] = 0xFF;
	f->pixels[(size_t)(h - 1) * w + (w - 2)] = 0xFF;
	f->pixels[(size_t)(h - 2) * w + (w - 1)] = 0xFF;
	f->pixels[(size_t)(h - 2) * w + (w - 2)] = 0xFF;

	/* Boundary between the two white columns/rows → averages white↔white. */
	f->white_u = (float)(w - 1) / (float)w;
	f->white_v = (float)(h - 1) / (float)h;
}

int kgui_font_bake(struct kgui_font *f, const uint8_t *ttf, int ttf_len)
{
	stbtt_fontinfo   *info;
	unsigned char    *pixels;
	stbtt_pack_range  pr[KGUI_FONT_MAX_RANGES];
	stbtt_pack_context spc;
	int               offset, ascent, descent, line_gap, i, ok;

	memset(f, 0, sizeof(*f));
	if (!ttf || ttf_len <= 0 || K_RANGE_COUNT > KGUI_FONT_MAX_RANGES)
		return 0;

	offset = stbtt_GetFontOffsetForIndex(ttf, 0);
	if (offset < 0)
		return 0;

	info = (stbtt_fontinfo *)malloc(sizeof(*info));
	if (!info)
		return 0;
	if (!stbtt_InitFont(info, ttf, offset)) {
		free(info);
		return 0;
	}

	pixels = (unsigned char *)malloc((size_t)KGUI_FONT_ATLAS_W *
					 (size_t)KGUI_FONT_ATLAS_H);
	if (!pixels) {
		free(info);
		return 0;
	}

	/* One packedchar table per range; wire up the pack_range descriptors. */
	memset(pr, 0, sizeof(pr));
	for (i = 0; i < K_RANGE_COUNT; i++) {
		stbtt_packedchar *pc = (stbtt_packedchar *)
			malloc((size_t)k_ranges[i].count * sizeof(*pc));

		if (!pc) {
			int j;

			for (j = 0; j < i; j++)
				free(pr[j].chardata_for_range);
			free(pixels);
			free(info);
			return 0;
		}
		pr[i].font_size                     = KGUI_FONT_BAKE_PX;
		pr[i].first_unicode_codepoint_in_range = (int)k_ranges[i].first;
		pr[i].array_of_unicode_codepoints   = NULL;
		pr[i].num_chars                     = k_ranges[i].count;
		pr[i].chardata_for_range            = pc;
	}

	if (!stbtt_PackBegin(&spc, pixels, KGUI_FONT_ATLAS_W, KGUI_FONT_ATLAS_H,
			     0, 1, NULL)) {
		for (i = 0; i < K_RANGE_COUNT; i++)
			free(pr[i].chardata_for_range);
		free(pixels);
		free(info);
		return 0;
	}
	stbtt_PackSetOversampling(&spc, 1, 1);
	ok = stbtt_PackFontRanges(&spc, ttf, 0, pr, K_RANGE_COUNT);
	stbtt_PackEnd(&spc);

	if (!ok) {
		/* A glyph range did not fit the atlas — fail loudly, no partial. */
		for (i = 0; i < K_RANGE_COUNT; i++)
			free(pr[i].chardata_for_range);
		free(pixels);
		free(info);
		return 0;
	}

	f->pixels  = pixels;
	f->atlas_w = KGUI_FONT_ATLAS_W;
	f->atlas_h = KGUI_FONT_ATLAS_H;
	f->bake_px = KGUI_FONT_BAKE_PX;
	for (i = 0; i < K_RANGE_COUNT; i++) {
		f->ranges[i].first  = k_ranges[i].first;
		f->ranges[i].count  = k_ranges[i].count;
		f->ranges[i].packed = pr[i].chardata_for_range;
	}
	f->range_count = K_RANGE_COUNT;

	reserve_white_texel(f);

	stbtt_GetFontVMetrics(info, &ascent, &descent, &line_gap);
	f->ascent_px = (float)ascent *
		       stbtt_ScaleForPixelHeight(info, KGUI_FONT_BAKE_PX);

	f->info  = info;
	f->ttf   = ttf;
	f->ready = 1;
	return 1;
}

/* Locate the packedchar for `cp`; returns NULL (with *index untouched) if none. */
static const stbtt_packedchar *find_glyph(const struct kgui_font *f,
					  uint32_t cp, int *index)
{
	int i;

	for (i = 0; i < f->range_count; i++) {
		const struct kgui_font_range *r = &f->ranges[i];

		if (cp >= r->first && (int)(cp - r->first) < r->count) {
			*index = (int)(cp - r->first);
			return (const stbtt_packedchar *)r->packed;
		}
	}
	return NULL;
}

int kgui_font_glyph(void *ud, uint32_t cp, float size, struct kgui_glyph *out)
{
	struct kgui_font       *f = (struct kgui_font *)ud;
	const stbtt_packedchar *pc;
	stbtt_aligned_quad      q;
	float                   xpos = 0.0f, ypos = 0.0f, s;
	int                     index = 0;

	if (!f || !f->ready)
		return 0;
	pc = find_glyph(f, cp, &index);
	if (!pc)
		return 0;

	/*
	 * GetPackedQuad places the glyph relative to a baseline pen at (0, 0):
	 * q spans the box in bake-pixel space (y negative above the baseline)
	 * and advances xpos by the glyph's advance. We shift by the ascent so
	 * (0, 0) becomes the text's top-left, then scale bake→requested size.
	 */
	stbtt_GetPackedQuad((stbtt_packedchar *)pc, f->atlas_w, f->atlas_h,
			    index, &xpos, &ypos, &q, 0);
	s = (f->bake_px > 0.0f) ? size / f->bake_px : 1.0f;

	out->x0      = q.x0 * s;
	out->y0      = (q.y0 + f->ascent_px) * s;
	out->x1      = q.x1 * s;
	out->y1      = (q.y1 + f->ascent_px) * s;
	out->u0      = q.s0;
	out->v0      = q.t0;
	out->u1      = q.s1;
	out->v1      = q.t1;
	out->advance = xpos * s;
	out->visible = (q.x1 > q.x0 && q.y1 > q.y0);
	return 1;
}

float kgui_font_kern(void *ud, uint32_t prev, uint32_t cur, float size)
{
	struct kgui_font *f = (struct kgui_font *)ud;
	int               adv;

	if (!f || !f->ready || !f->info)
		return 0.0f;
	adv = stbtt_GetCodepointKernAdvance((stbtt_fontinfo *)f->info,
					    (int)prev, (int)cur);
	if (adv == 0)
		return 0.0f;
	return (float)adv *
	       stbtt_ScaleForPixelHeight((stbtt_fontinfo *)f->info, size);
}
