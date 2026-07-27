/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Host tests for the GL-free geometry core (kgui_batch). They exercise the
 * vertex output for a filled rect and a text run without a GL context: a fake
 * glyph source stands in for the ImGui font atlas so layout and vertex counts
 * are checked deterministically.
 */

#include "kgui_batch.h"

#include <assert.h>
#include <math.h>
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

static void test_image_emits_uv_tint_and_texture(void)
{
	struct kgui_batch b;

	kgui_batch_init(&b, verts, STORAGE);
	kgui_batch_image(&b, 10.0f, 20.0f, 30.0f, 40.0f,
			 0.25f, 0.5f, 0.75f, 1.0f, 7u,
			 1.0f, 1.0f, 1.0f, 0.5f);

	assert(b.count == 6);
	assert(b.cmd_count == 1);
	assert(b.cmds[0].tex == 7u);   /* the command carries the texture */
	assert(!b.cmds[0].clipped);

	/* The quad spans the box with the given sub-rect UVs at the corners. */
	assert(verts[0].x == 10.0f && verts[0].y == 20.0f);
	assert(verts[0].u == 0.25f && verts[0].v == 0.5f);
	assert(verts[2].x == 40.0f && verts[2].y == 60.0f);
	assert(verts[2].u == 0.75f && verts[2].v == 1.0f);
	/* The tint rides on every vertex (here a half-alpha fade). */
	assert(verts[0].r == 1.0f && verts[0].a == 0.5f);
}

static void test_image_splits_from_atlas_commands(void)
{
	struct kgui_batch b;

	kgui_batch_init(&b, verts, STORAGE);
	kgui_batch_quad(&b, 0, 0, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1);  /* atlas   */
	kgui_batch_image(&b, 0, 0, 1, 1, 0, 0, 1, 1, 9u, 1, 1, 1, 1); /* image */
	kgui_batch_quad(&b, 0, 0, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1);  /* atlas   */

	/* The texture change opens a command each way: atlas / image / atlas. */
	assert(b.cmd_count == 3);
	assert(b.cmds[0].tex == 0u);
	assert(b.cmds[1].tex == 9u);
	assert(b.cmds[1].first == 6 && b.cmds[1].count == 6);
	assert(b.cmds[2].tex == 0u);
}

static void test_image_same_texture_shares_command(void)
{
	struct kgui_batch b;

	kgui_batch_init(&b, verts, STORAGE);
	kgui_batch_image(&b, 0, 0, 1, 1, 0, 0, 1, 1, 5u, 1, 1, 1, 1);
	kgui_batch_image(&b, 2, 0, 1, 1, 0, 0, 1, 1, 5u, 1, 1, 1, 1);
	assert(b.cmd_count == 1);            /* same texture -> one run   */
	assert(b.cmds[0].count == 12);

	kgui_batch_image(&b, 4, 0, 1, 1, 0, 0, 1, 1, 6u, 1, 1, 1, 1);
	assert(b.cmd_count == 2);            /* a new texture splits it   */
	assert(b.cmds[1].tex == 6u);
}

static void test_image_honours_clip(void)
{
	struct kgui_batch b;

	kgui_batch_init(&b, verts, STORAGE);
	kgui_batch_set_clip(&b, 10, 20, 30, 40);
	kgui_batch_image(&b, 0, 0, 1, 1, 0, 0, 1, 1, 8u, 1, 1, 1, 1);

	assert(b.cmd_count == 1);
	assert(b.cmds[0].clipped);
	assert(b.cmds[0].x == 10 && b.cmds[0].y == 20);
	assert(b.cmds[0].tex == 8u);
}

static void test_reset_clears_texture(void)
{
	struct kgui_batch b;

	kgui_batch_init(&b, verts, STORAGE);
	kgui_batch_image(&b, 0, 0, 1, 1, 0, 0, 1, 1, 3u, 1, 1, 1, 1);
	assert(b.cmds[0].tex == 3u);

	kgui_batch_reset(&b);
	/* After an image + reset, the next atlas quad is back on texture 0. */
	kgui_batch_quad(&b, 0, 0, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1);
	assert(b.cmd_count == 1 && b.cmds[0].tex == 0u);
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

/* A thick horizontal segment is a rotated quad: two triangles, six vertices,
 * offset perpendicular by half the width, on the atlas (untextured) path. */
static void test_line_emits_rotated_quad(void)
{
	struct kgui_batch b;

	kgui_batch_init(&b, verts, STORAGE);
	kgui_batch_line(&b, 0.0f, 0.0f, 10.0f, 0.0f, 4.0f, 0.5f, 0.5f,
			1.0f, 0.2f, 0.3f, 1.0f);

	assert(b.count == 6 && !b.overflow);
	/* Horizontal line, width 4 -> corners offset +/-2 in y. */
	assert(verts[0].x == 0.0f && verts[0].y == 2.0f);
	assert(verts[1].x == 10.0f && verts[1].y == 2.0f);
	assert(verts[2].x == 10.0f && verts[2].y == -2.0f);
	/* The white texel UV and colour ride on every vertex. */
	assert(verts[0].u == 0.5f && verts[0].v == 0.5f);
	assert(verts[0].r == 1.0f && verts[0].a == 1.0f);
	/* Untextured: shares the atlas command with rects and glyphs. */
	assert(b.cmd_count == 1 && b.cmds[0].tex == 0u);
}

/* A zero-length segment has no direction and draws nothing. */
static void test_line_degenerate_noop(void)
{
	struct kgui_batch b;

	kgui_batch_init(&b, verts, STORAGE);
	kgui_batch_line(&b, 5.0f, 5.0f, 5.0f, 5.0f, 3.0f, 0.0f, 0.0f,
			1.0f, 1.0f, 1.0f, 1.0f);
	assert(b.count == 0);
}

/* A filled circle is a centre-fan: segs triangles, each rim point on radius. */
static void test_circle_fan_count_and_radius(void)
{
	struct kgui_batch b;
	int               i;

	kgui_batch_init(&b, verts, STORAGE);
	kgui_batch_circle(&b, 100.0f, 100.0f, 10.0f, 12, 0.5f, 0.5f,
			  1.0f, 1.0f, 1.0f, 1.0f);
	assert(b.count == 36); /* 12 triangles */
	for (i = 0; i < 12; i++) {
		float rx = verts[i * 3 + 1].x - 100.0f;
		float ry = verts[i * 3 + 1].y - 100.0f;

		assert(verts[i * 3].x == 100.0f && verts[i * 3].y == 100.0f);
		assert(fabsf(sqrtf(rx * rx + ry * ry) - 10.0f) < 1e-3f);
	}
}

/* Fewer than three segments is clamped up to a triangle. */
static void test_circle_clamps_min_segments(void)
{
	struct kgui_batch b;

	kgui_batch_init(&b, verts, STORAGE);
	kgui_batch_circle(&b, 0.0f, 0.0f, 5.0f, 1, 0.0f, 0.0f,
			  1.0f, 1.0f, 1.0f, 1.0f);
	assert(b.count == 9); /* clamped to 3 triangles */
}

/* A ring is a strip of quads between the inner and outer radii. */
static void test_ring_quad_count_and_radii(void)
{
	struct kgui_batch b;
	float             d;

	kgui_batch_init(&b, verts, STORAGE);
	kgui_batch_ring(&b, 0.0f, 0.0f, 10.0f, 2.0f, 8, 0.5f, 0.5f,
			1.0f, 1.0f, 1.0f, 1.0f);
	assert(b.count == 48); /* 8 quads * 6 vertices */
	/* First vertex on the outer radius (10 + 1), third on the inner (10 - 1). */
	d = sqrtf(verts[0].x * verts[0].x + verts[0].y * verts[0].y);
	assert(fabsf(d - 11.0f) < 1e-3f);
	d = sqrtf(verts[2].x * verts[2].x + verts[2].y * verts[2].y);
	assert(fabsf(d - 9.0f) < 1e-3f);
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
	RUN(image_emits_uv_tint_and_texture);
	RUN(image_splits_from_atlas_commands);
	RUN(image_same_texture_shares_command);
	RUN(image_honours_clip);
	RUN(reset_clears_texture);
	RUN(utf8_decode);
	RUN(line_emits_rotated_quad);
	RUN(line_degenerate_noop);
	RUN(circle_fan_count_and_radius);
	RUN(circle_clamps_min_segments);
	RUN(ring_quad_count_and_radii);

	printf("%d/%d tests passed\n", tests_passed, tests_run);
	return tests_passed == tests_run ? 0 : 1;
}
