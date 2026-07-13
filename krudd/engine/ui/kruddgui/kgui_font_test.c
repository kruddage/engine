/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Host tests for the glyph baker (kgui_font). Unlike kgui_batch's tests these
 * need a real TrueType font to exercise stb_truetype, so the font is supplied
 * at runtime rather than embedded: pass a path as argv[1] or in KGUI_TEST_FONT.
 * With no font — as in the default CI run — the suite prints SKIP and succeeds,
 * so it never blocks a build that has not yet vendored the UI font; point it at
 * any .ttf to get real coverage of the bake, glyph metrics, and kerning.
 */

#include "kgui_font.h"
#include "kgui_batch.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static int tests_run;
static int tests_passed;

#define RUN(name) do {                 \
	tests_run++;                   \
	test_##name();                 \
	tests_passed++;                \
	printf("PASS: " #name "\n");   \
} while (0)

static struct kgui_font g_font;

static void test_bake_succeeds(void)
{
	assert(g_font.ready);
	assert(g_font.pixels != NULL);
	assert(g_font.atlas_w == KGUI_FONT_ATLAS_W);
	assert(g_font.atlas_h == KGUI_FONT_ATLAS_H);
	assert(g_font.bake_px == KGUI_FONT_BAKE_PX);
	assert(g_font.ascent_px > 0.0f);
}

static void test_white_texel_is_opaque(void)
{
	int w  = g_font.atlas_w;
	int h  = g_font.atlas_h;
	int px = (int)(g_font.white_u * (float)w);
	int py = (int)(g_font.white_v * (float)h);

	/* The reserved corner samples solid coverage for kgui-rect fills. */
	assert(g_font.pixels[(size_t)py * w + px] == 0xFF);
}

static void test_visible_glyph_has_atlas_box(void)
{
	struct kgui_glyph gl;

	assert(kgui_font_glyph(&g_font, 'A', 20.0f, &gl));
	assert(gl.visible);
	assert(gl.advance > 0.0f);
	assert(gl.x1 > gl.x0 && gl.y1 > gl.y0);
	/* Anchored top-left: a cap sits at or below the text top. */
	assert(gl.y0 >= -1.0f);
	/* UVs are normalised into the atlas. */
	assert(gl.u0 >= 0.0f && gl.u1 <= 1.0f);
	assert(gl.v0 >= 0.0f && gl.v1 <= 1.0f);
	assert(gl.u1 > gl.u0 && gl.v1 > gl.v0);
}

static void test_space_advances_without_ink(void)
{
	struct kgui_glyph gl;

	assert(kgui_font_glyph(&g_font, ' ', 20.0f, &gl));
	assert(!gl.visible);          /* no drawable box */
	assert(gl.advance > 0.0f);    /* but still advances the pen */
}

static void test_advance_scales_with_size(void)
{
	struct kgui_glyph a, b;

	assert(kgui_font_glyph(&g_font, 'M', 20.0f, &a));
	assert(kgui_font_glyph(&g_font, 'M', 40.0f, &b));
	/* Double the pixel size → double the advance (within rounding). */
	assert(b.advance > a.advance * 1.9f && b.advance < a.advance * 2.1f);
}

static void test_uncovered_codepoint_has_no_glyph(void)
{
	struct kgui_glyph gl;

	/* U+4E00 (CJK) is outside the baked Latin ranges. */
	assert(!kgui_font_glyph(&g_font, 0x4E00u, 20.0f, &gl));
}

static void test_kerning_never_widens(void)
{
	float plain  = kgui_text_width("AVATAR Wave", 20.0f,
				       kgui_font_glyph, NULL, &g_font);
	float kerned = kgui_text_width("AVATAR Wave", 20.0f,
				       kgui_font_glyph, kgui_font_kern, &g_font);

	/* Kerning only ever tucks glyphs closer, so it cannot widen the run. */
	assert(kerned <= plain);
}

static void test_measured_matches_drawn(void)
{
	static struct kgui_vertex verts[512];
	struct kgui_batch         b;
	float                     drawn, measured;

	kgui_batch_init(&b, verts, 512);
	drawn = kgui_batch_text(&b, 0.0f, 0.0f, "Kerning", 20.0f,
				1.0f, 1.0f, 1.0f, 1.0f,
				kgui_font_glyph, kgui_font_kern, &g_font);
	measured = kgui_text_width("Kerning", 20.0f,
				   kgui_font_glyph, kgui_font_kern, &g_font);
	assert(drawn == measured);
}

static unsigned char *read_file(const char *path, int *len)
{
	FILE          *fp = fopen(path, "rb");
	long           n;
	unsigned char *buf;

	if (!fp)
		return NULL;
	fseek(fp, 0, SEEK_END);
	n = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if (n <= 0) {
		fclose(fp);
		return NULL;
	}
	buf = (unsigned char *)malloc((size_t)n);
	if (buf && fread(buf, 1, (size_t)n, fp) != (size_t)n) {
		free(buf);
		buf = NULL;
	}
	fclose(fp);
	if (buf)
		*len = (int)n;
	return buf;
}

int main(int argc, char **argv)
{
	const char    *path = (argc > 1) ? argv[1] : getenv("KGUI_TEST_FONT");
	unsigned char *ttf;
	int            len = 0;

	if (!path) {
		printf("SKIP: no font (pass a .ttf path or set KGUI_TEST_FONT)\n");
		return 0;
	}
	ttf = read_file(path, &len);
	if (!ttf) {
		fprintf(stderr, "kgui_font_test: cannot read font '%s'\n", path);
		return 1;
	}
	if (!kgui_font_bake(&g_font, ttf, len)) {
		fprintf(stderr, "kgui_font_test: bake failed for '%s'\n", path);
		free(ttf);
		return 1;
	}

	RUN(bake_succeeds);
	RUN(white_texel_is_opaque);
	RUN(visible_glyph_has_atlas_box);
	RUN(space_advances_without_ink);
	RUN(advance_scales_with_size);
	RUN(uncovered_codepoint_has_no_glyph);
	RUN(kerning_never_widens);
	RUN(measured_matches_drawn);

	kgui_font_free(&g_font);
	free(ttf);

	printf("%d/%d tests passed\n", tests_passed, tests_run);
	return tests_passed == tests_run ? 0 : 1;
}
