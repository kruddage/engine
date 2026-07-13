/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef KGUI_FONT_H
#define KGUI_FONT_H

#include <stdint.h>

#include "kgui_batch.h" /* struct kgui_glyph, kgui_glyph_fn, kgui_kern_fn */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * kgui_font — kruddgui's own glyph pipeline, replacing the borrowed Dear ImGui
 * font atlas.
 *
 * It bakes a single-channel (R8 coverage) glyph atlas from a TrueType font with
 * stb_truetype and answers the two seams kgui_batch draws through: a
 * kgui_glyph_fn (per-code-point box + atlas UVs + advance) and a kgui_kern_fn
 * (per-pair pen adjustment from the font's kern table). This half is GL-free
 * and host-testable — it produces the atlas *bitmap* in CPU memory; uploading
 * that bitmap as a texture, and drawing through it, stays in kruddgui.cpp.
 *
 * Baking once at a supersampled pixel size (see KGUI_FONT_BAKE_PX) and drawing
 * minified keeps labels crisp at any device-pixel-ratio without a per-DPR
 * rebake — the atlas is always sampled down, never up, on the displays we
 * target. That supersede the old ImGui-atlas upscale that left text soft.
 */

/*
 * Pixel size the glyphs are rasterised at. Chosen a comfortable margin above
 * the largest on-screen glyph height we expect (mode-bar labels drawn at
 * ~19.5 CSS px, times a device-pixel-ratio up to ~3 → ~58 physical px), so the
 * atlas is minified rather than magnified when sampled. Bigger = sharper but
 * more atlas area.
 */
#define KGUI_FONT_BAKE_PX 64.0f

/*
 * Atlas dimensions. Square, power-of-two, sized to hold the baked ranges below
 * at KGUI_FONT_BAKE_PX with room for stb's packing plus the reserved white
 * texel. Basic Latin + Latin-1 Supplement (~200 glyphs) at 64px fits inside
 * 1024×1024 with slack; kgui_font_bake fails loudly (returns 0) if a glyph
 * range does not fit rather than dropping glyphs silently.
 */
#define KGUI_FONT_ATLAS_W 1024
#define KGUI_FONT_ATLAS_H 1024

/*
 * One contiguous run of code points baked into the atlas, and the stb packing
 * record for each. Kept per range so glyph lookup is a range scan then an index
 * — no full 0..0x10FFFF table.
 */
struct kgui_font_range {
	uint32_t first;      /* first code point in the run           */
	int      count;      /* number of code points                 */
	void    *packed;     /* stbtt_packedchar[count], owned        */
};

#define KGUI_FONT_MAX_RANGES 4

struct kgui_font {
	int   ready;

	/* Baked atlas bitmap: one byte of coverage per texel, owned. */
	unsigned char *pixels;
	int            atlas_w, atlas_h;

	float bake_px;      /* size the glyphs were rasterised at        */
	float ascent_px;    /* baseline offset from the text top, bake px */

	/* Reserved solid-white texel, for kgui-rect's textured fills. */
	float white_u, white_v;

	struct kgui_font_range ranges[KGUI_FONT_MAX_RANGES];
	int                    range_count;

	/*
	 * Retained font handle + source for kerning queries (stb's packer does
	 * not expose one). `info` is an stbtt_fontinfo; `ttf` must outlive the
	 * font (in kruddgui it is the static embedded blob).
	 */
	void          *info;   /* heap stbtt_fontinfo, owned */
	const uint8_t *ttf;    /* not owned                  */
};

/*
 * Bake `ttf` (a TrueType blob of `ttf_len` bytes) into `f`'s atlas at
 * KGUI_FONT_BAKE_PX, covering Basic Latin and the Latin-1 Supplement. Allocates
 * f->pixels and the per-range packing tables. Returns 1 on success, 0 on a bad
 * font, an allocation failure, or a range that does not fit the atlas — on
 * failure `f` is left zeroed and safe to pass to the glyph/kern sources (they
 * report "no glyph" / no kerning). Call kgui_font_free to release it.
 *
 * The blob is trusted, compile-time-embedded engine data, never a user- or
 * network-supplied file — see the trust-boundary note in third_party/VENDOR.md.
 */
int kgui_font_bake(struct kgui_font *f, const uint8_t *ttf, int ttf_len);

void kgui_font_free(struct kgui_font *f);

/*
 * kgui_glyph_fn over a kgui_font*: map a code point to its baked atlas quad,
 * scaled from the bake size to the requested pixel size, anchored so (x, y) is
 * the text's top-left (matching the advance-only layout kgui_batch expects).
 */
int kgui_font_glyph(void *ud, uint32_t cp, float size, struct kgui_glyph *out);

/*
 * kgui_kern_fn over a kgui_font*: the font's kern-table pen adjustment between
 * `prev` and `cur` at `size` (pixels, usually negative). 0 when the font has no
 * pair or no kern table.
 */
float kgui_font_kern(void *ud, uint32_t prev, uint32_t cur, float size);

#ifdef __cplusplus
}
#endif

#endif /* KGUI_FONT_H */
