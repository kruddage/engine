/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Host tests for the GL-free geometry core (kgui_batch). They exercise the
 * vertex output for a filled rect and a text run without a GL context: a fake
 * glyph source stands in for the ImGui font atlas so layout and vertex counts
 * are checked deterministically.
 */

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

/*
 * Fake font: every glyph is a `size`-wide, `size`-tall box advancing by `size`,
 * except ' ' which is invisible whitespace of the same advance, and '\t' which
 * has no glyph at all (the source returns 0). Enough shape to assert positions,
 * the visible/whitespace split, and the missing-glyph skip.
 */
static int fake_glyph(void *ud, uint32_t cp, float size, struct kgui_glyph *out)
{
	(void)ud;
	if (cp == '\t')
		return 0;

	out->x0      = 0.0f;
	out->y0      = 0.0f;
	out->x1      = size;
	out->y1      = size;
	out->u0      = 0.0f;
	out->v0      = 0.0f;
	out->u1      = 1.0f;
	out->v1      = 1.0f;
	out->advance = size;
	out->visible = (cp != ' ');
	return 1;
}

#define STORAGE 256
static struct kgui_vertex verts[STORAGE];

static void test_rect_emits_six_vertices(void)
{
	struct kgui_batch b;

	kgui_batch_init(&b, verts, STORAGE);
	kgui_batch_quad(&b, 10.0f, 20.0f, 30.0f, 40.0f,
			0.0f, 0.0f, 0.0f, 0.0f,
			1.0f, 0.5f, 0.25f, 1.0f);

	assert(b.count == 6);
	assert(!b.overflow);

	/* First vertex is the top-left corner with the pushed colour. */
	assert(verts[0].x == 10.0f && verts[0].y == 20.0f);
	assert(verts[0].r == 1.0f && verts[0].g == 0.5f &&
	       verts[0].b == 0.25f && verts[0].a == 1.0f);

	/* The two triangles between them span the far corner (40, 60). */
	assert(verts[2].x == 40.0f && verts[2].y == 60.0f);
	assert(verts[4].x == 40.0f && verts[4].y == 60.0f);
	/* Last vertex closes on the bottom-left corner. */
	assert(verts[5].x == 10.0f && verts[5].y == 60.0f);
}

static void test_text_run_vertices_and_width(void)
{
	struct kgui_batch b;
	float             w;

	kgui_batch_init(&b, verts, STORAGE);
	/* "AB": two visible glyphs -> two quads -> 12 vertices. */
	w = kgui_batch_text(&b, 100.0f, 200.0f, "AB", 8.0f,
			    1.0f, 1.0f, 1.0f, 1.0f, fake_glyph, NULL);

	assert(b.count == 12);
	assert(w == 16.0f); /* two glyphs advancing 8px each */

	/* Second glyph starts one advance to the right of the first. */
	assert(verts[0].x == 100.0f);
	assert(verts[6].x == 108.0f);
	assert(verts[0].y == 200.0f);
}

static void test_whitespace_advances_without_quad(void)
{
	struct kgui_batch b;
	float             w;

	kgui_batch_init(&b, verts, STORAGE);
	/* "A B": space is invisible -> only two quads, but full advance. */
	w = kgui_batch_text(&b, 0.0f, 0.0f, "A B", 10.0f,
			    1.0f, 1.0f, 1.0f, 1.0f, fake_glyph, NULL);

	assert(b.count == 12); /* 'A' and 'B' only */
	assert(w == 30.0f);    /* three advances of 10px */
	/* 'B' sits after the letter and the space: two advances in. */
	assert(verts[6].x == 20.0f);
}

static void test_missing_glyph_skipped(void)
{
	struct kgui_batch b;
	float             w;

	kgui_batch_init(&b, verts, STORAGE);
	/* '\t' has no glyph: it neither draws nor advances. */
	w = kgui_batch_text(&b, 0.0f, 0.0f, "A\tB", 8.0f,
			    1.0f, 1.0f, 1.0f, 1.0f, fake_glyph, NULL);

	assert(b.count == 12);
	assert(w == 16.0f);
}

static void test_width_matches_no_geometry(void)
{
	struct kgui_batch b;
	float             drawn, measured;

	kgui_batch_init(&b, verts, STORAGE);
	drawn    = kgui_batch_text(&b, 5.0f, 5.0f, "Hello", 12.0f,
				   1.0f, 1.0f, 1.0f, 1.0f, fake_glyph, NULL);
	measured = kgui_text_width("Hello", 12.0f, fake_glyph, NULL);

	assert(drawn == measured);
	assert(measured == 60.0f); /* 5 glyphs * 12px */
}

static void test_overflow_latches_and_bounds(void)
{
	struct kgui_batch b;
	struct kgui_vertex small[6];

	kgui_batch_init(&b, small, 6);
	kgui_batch_quad(&b, 0, 0, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1); /* fills it */
	assert(b.count == 6 && !b.overflow);

	kgui_batch_quad(&b, 0, 0, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1); /* one past */
	assert(b.overflow);
	assert(b.count == 6); /* never wrote past capacity */
}

static void test_unclipped_is_one_command(void)
{
	struct kgui_batch b;

	kgui_batch_init(&b, verts, STORAGE);
	kgui_batch_quad(&b, 0, 0, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1);
	kgui_batch_quad(&b, 2, 0, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1);

	/* No clip set: both quads share one full-viewport command. */
	assert(b.cmd_count == 1);
	assert(!b.cmds[0].clipped);
	assert(b.cmds[0].first == 0 && b.cmds[0].count == 12);
}

static void test_clip_splits_commands(void)
{
	struct kgui_batch b;

	kgui_batch_init(&b, verts, STORAGE);
	kgui_batch_quad(&b, 0, 0, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1); /* unclipped */
	kgui_batch_set_clip(&b, 10, 20, 30, 40);
	kgui_batch_quad(&b, 0, 0, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1); /* clipped   */
	kgui_batch_quad(&b, 2, 0, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1); /* same clip */
	kgui_batch_clear_clip(&b);
	kgui_batch_quad(&b, 4, 0, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1); /* unclipped */

	assert(b.cmd_count == 3);

	assert(!b.cmds[0].clipped);
	assert(b.cmds[0].first == 0 && b.cmds[0].count == 6);

	assert(b.cmds[1].clipped);
	assert(b.cmds[1].x == 10 && b.cmds[1].y == 20 &&
	       b.cmds[1].w == 30 && b.cmds[1].h == 40);
	assert(b.cmds[1].first == 6 && b.cmds[1].count == 12); /* two quads */

	assert(!b.cmds[2].clipped);
	assert(b.cmds[2].first == 18 && b.cmds[2].count == 6);
}

static void test_reset_clears_clip_and_commands(void)
{
	struct kgui_batch b;

	kgui_batch_init(&b, verts, STORAGE);
	kgui_batch_set_clip(&b, 1, 2, 3, 4);
	kgui_batch_quad(&b, 0, 0, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1);
	assert(b.cmd_count == 1 && b.cmds[0].clipped);

	kgui_batch_reset(&b);
	assert(b.cmd_count == 0);

	/* The clip did not survive the reset: the next quad is unclipped. */
	kgui_batch_quad(&b, 0, 0, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1);
	assert(b.cmd_count == 1 && !b.cmds[0].clipped);
}

static void test_utf8_decode(void)
{
	/* 'A', 'é' (U+00E9, 2 bytes), '€' (U+20AC, 3 bytes). */
	const char *s = "A\xC3\xA9\xE2\x82\xAC";

	assert(kgui_utf8_next(&s) == 0x41);
	assert(kgui_utf8_next(&s) == 0xE9);
	assert(kgui_utf8_next(&s) == 0x20AC);
	assert(*s == '\0');
}

int main(void)
{
	RUN(rect_emits_six_vertices);
	RUN(text_run_vertices_and_width);
	RUN(whitespace_advances_without_quad);
	RUN(missing_glyph_skipped);
	RUN(width_matches_no_geometry);
	RUN(overflow_latches_and_bounds);
	RUN(unclipped_is_one_command);
	RUN(clip_splits_commands);
	RUN(reset_clears_clip_and_commands);
	RUN(utf8_decode);

	printf("%d/%d tests passed\n", tests_passed, tests_run);
	return tests_passed == tests_run ? 0 : 1;
}
