/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Host tests for the GL-free glyph atlas (kgui_font) — the owned font pipeline
 * that bakes JetBrains Mono into an SDF atlas with stb_truetype. They run the
 * whole bake with no GL context and assert on the atlas layout (a real distance
 * field, the solid "inside" texel, every glyph inside its cell), on per-glyph
 * metrics (visibility, monospace advance, size scaling, in-bounds UVs), and that
 * the glyph source drives kgui_batch_text the way kruddgui.cpp will.
 */

#include "kgui_font.h"
#include "kgui_batch.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int tests_run;
static int tests_passed;

#define RUN(name) do {                 \
	tests_run++;                   \
	test_##name();                 \
	tests_passed++;                \
	printf("PASS: " #name "\n");   \
} while (0)

/* One shared baked font: init is deterministic, so the tests can share it. */
static struct kgui_font font;

static const struct kgui_font_metric *metric_of(uint32_t cp)
{
	return &font.metrics[cp - KGUI_FONT_FIRST];
}

static void test_inside_texel_is_opaque_white(void)
{
	int x = (int)(font.white_u * KGUI_FONT_ATLAS_W);
	int y = (int)(font.white_v * KGUI_FONT_ATLAS_H);
	const unsigned char *px =
		&font.pixels[(y * KGUI_FONT_ATLAS_W + x) * 4];

	/* rgb white, alpha fully "inside" — a filled rect resolves to solid. */
	assert(px[0] == 255 && px[1] == 255 && px[2] == 255 && px[3] == 255);
}

/*
 * Colour comes from the vertex, so every texel is white in RGB; the alpha is a
 * signed distance field — it must carry intermediate values, not just 0/255,
 * or it is not a distance field at all.
 */
static void test_atlas_is_a_distance_field(void)
{
	int n = KGUI_FONT_ATLAS_W * KGUI_FONT_ATLAS_H;
	int i, saw_edge = 0, saw_ramp = 0;

	for (i = 0; i < n; i++) {
		const unsigned char *px = &font.pixels[i * 4];

		assert(px[0] == 255 && px[1] == 255 && px[2] == 255);
		if (px[3] == KGUI_FONT_SDF_ONEDGE)
			saw_edge = 1;
		if (px[3] > 0 && px[3] < 255)
			saw_ramp = 1;
	}
	assert(saw_edge); /* the outline value shows up */
	assert(saw_ramp); /* and a genuine ramp around it */
}

/* Every visible glyph's SDF bitmap fits inside its atlas cell (no clipping). */
static void test_every_glyph_fits_its_cell(void)
{
	uint32_t cp;

	for (cp = KGUI_FONT_FIRST; cp <= KGUI_FONT_LAST; cp++) {
		const struct kgui_font_metric *m = metric_of(cp);

		assert(m->w <= KGUI_FONT_CELL_W);
		assert(m->h <= KGUI_FONT_CELL_H);
	}
}

static void test_space_is_blank_but_advances(void)
{
	const struct kgui_font_metric *m = metric_of(' ');
	struct kgui_glyph g;

	assert(m->visible == 0);
	assert(kgui_font_glyph(&font, ' ', 16.0f, &g) == 1);
	assert(g.visible == 0);
	assert(g.advance > 0.0f);
}

static void test_letter_is_visible_and_boxed(void)
{
	struct kgui_glyph g;

	assert(kgui_font_glyph(&font, 'A', 16.0f, &g) == 1);
	assert(g.visible == 1);
	assert(g.x1 > g.x0); /* non-degenerate box */
	assert(g.y1 > g.y0);
	assert(g.advance > 0.0f);
	/* UVs inside the atlas, non-degenerate. */
	assert(g.u0 >= 0.0f && g.u1 <= 1.0f && g.u0 < g.u1);
	assert(g.v0 >= 0.0f && g.v1 <= 1.0f && g.v0 < g.v1);
}

/* Box and advance scale linearly with the requested pixel size. */
static void test_metrics_scale_with_size(void)
{
	struct kgui_glyph a, b;

	kgui_font_glyph(&font, 'A', 16.0f, &a);
	kgui_font_glyph(&font, 'A', 32.0f, &b);
	assert(b.x0 == a.x0 * 2.0f);
	assert(b.y0 == a.y0 * 2.0f);
	assert(b.x1 == a.x1 * 2.0f);
	assert(b.y1 == a.y1 * 2.0f);
	assert(b.advance == a.advance * 2.0f);
	/* Same atlas cell — UVs do not move with size. */
	assert(b.u0 == a.u0 && b.v0 == a.v0);
}

/* The face is monospace: every glyph shares one advance. */
static void test_font_is_monospace(void)
{
	struct kgui_glyph i, w, space;

	kgui_font_glyph(&font, 'i', 16.0f, &i);
	kgui_font_glyph(&font, 'W', 16.0f, &w);
	kgui_font_glyph(&font, ' ', 16.0f, &space);
	assert(i.advance == w.advance);
	assert(i.advance == space.advance);
}

static void test_unknown_codepoint_has_no_glyph(void)
{
	struct kgui_glyph g;

	assert(kgui_font_glyph(&font, '\n', 16.0f, &g) == 0);   /* below range */
	assert(kgui_font_glyph(&font, 0x7F, 16.0f, &g) == 0);   /* DEL, above range */
	assert(kgui_font_glyph(&font, 0x2603, 16.0f, &g) == 0); /* snowman */
	assert(kgui_font_glyph(NULL, 'A', 16.0f, &g) == 0);     /* no font */
}

/* kgui_batch_text emits six vertices per visible glyph and a positive width. */
static void test_drives_batch_text(void)
{
	struct kgui_vertex verts[256];
	struct kgui_batch  b;
	float              w;

	kgui_batch_init(&b, verts, 256);
	w = kgui_batch_text(&b, 0.0f, 0.0f, "Hi", KGUI_FONT_SIZE,
			    1.0f, 1.0f, 1.0f, 1.0f, kgui_font_glyph, &font);
	assert(b.count == 12); /* both glyphs visible */
	assert(w > 0.0f);

	/* A trailing space adds advance but no geometry. */
	kgui_batch_init(&b, verts, 256);
	w = kgui_batch_text(&b, 0.0f, 0.0f, "Hi ", KGUI_FONT_SIZE,
			    1.0f, 1.0f, 1.0f, 1.0f, kgui_font_glyph, &font);
	assert(b.count == 12);
	assert(w > 0.0f);
}

/* kgui_text_width grows with each added character (the caret placement path). */
static void test_text_width_is_monotonic(void)
{
	float a = kgui_text_width("a", KGUI_FONT_SIZE, kgui_font_glyph, &font);
	float b = kgui_text_width("ab", KGUI_FONT_SIZE, kgui_font_glyph, &font);
	float c = kgui_text_width("abc", KGUI_FONT_SIZE, kgui_font_glyph, &font);

	assert(a > 0.0f && b > a && c > b);
}

int main(void)
{
	kgui_font_init(&font);

	RUN(inside_texel_is_opaque_white);
	RUN(atlas_is_a_distance_field);
	RUN(every_glyph_fits_its_cell);
	RUN(space_is_blank_but_advances);
	RUN(letter_is_visible_and_boxed);
	RUN(metrics_scale_with_size);
	RUN(font_is_monospace);
	RUN(unknown_codepoint_has_no_glyph);
	RUN(drives_batch_text);
	RUN(text_width_is_monotonic);

	printf("\n%d/%d kgui font tests passed\n", tests_passed, tests_run);
	return tests_passed == tests_run ? 0 : 1;
}
