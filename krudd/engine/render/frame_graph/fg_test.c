/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "fg.h"
#include "renderer.h"
#include "renderer_null.h"
#include "subsystem_manager.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

static int tests_run;
static int tests_passed;

#define RUN(name) do { \
	tests_run++; \
	test_##name(); \
	tests_passed++; \
	printf("PASS: " #name "\n"); \
} while (0)

void renderer_null_plugin_entry(struct subsystem_manager *mgr);

static const struct subsystem empty_table[] = {{ NULL }};
static struct subsystem_manager mgr;
static struct fg *g_fg;

static void setup(void)
{
	const struct gpu_api *gpu;

	subsystem_manager_init(&mgr, empty_table);
	renderer_null_plugin_entry(&mgr); /* register renderer_null as "renderer" */
	renderer_null_reset_log();

	gpu  = subsystem_manager_get_api(&mgr, "renderer");
	assert(gpu != NULL);
	g_fg = fg_create(gpu);
	assert(g_fg != NULL);
}

static void teardown(void)
{
	fg_destroy(g_fg);
	subsystem_manager_shutdown(&mgr);
}

/* -------------------------------------------------------------------------
 * Sentinel draw callbacks — pass identity encoded in index_count.
 *
 * Each callback records a real draw through the lent command context: it
 * asks for the GPU api and command buffer, issues the call, and lets them
 * go. It never caches the context (borrowed, not kept).
 * -------------------------------------------------------------------------
 */
static void pass_draw(struct fg_pass_ctx *ctx, uint32_t index_count)
{
	const struct gpu_api *gpu = fg_ctx_gpu(ctx);
	struct gpu_draw_indexed_args draw = { .index_count = index_count };

	gpu->cmd_draw_indexed(fg_ctx_cmd(ctx), &draw);
}
static void pass_a_cb(struct fg_pass_ctx *ctx, void *ud)
{
	(void)ud;
	pass_draw(ctx, 1);
}
static void pass_b_cb(struct fg_pass_ctx *ctx, void *ud)
{
	(void)ud;
	pass_draw(ctx, 2);
}
static void pass_c_cb(struct fg_pass_ctx *ctx, void *ud)
{
	(void)ud;
	pass_draw(ctx, 3);
}

/* -------------------------------------------------------------------------
 * Test: topological ordering
 *
 * Declare passes A → B → C with resource dependencies.
 * The call log must show index_count 1, 2, 3 in that order.
 * -------------------------------------------------------------------------
 */
static void test_topological_ordering(void)
{
	const struct gpu_call_record *log;
	fg_resource_t r1, r2;
	fg_pass_t pa, pb, pc;
	uint32_t count, i, draw_idx;
	fg_tex_desc td = {
		.format = GPU_FORMAT_RGBA8_UNORM,
		.width = 1, .height = 1,
		.mip_levels = 1, .sample_count = 1,
	};

	setup();

	r1 = fg_declare_transient(g_fg, "R1", td);
	r2 = fg_declare_transient(g_fg, "R2", td);

	pa = fg_pass_declare(g_fg, "A", NULL, 0, &r1, 1);
	pb = fg_pass_declare(g_fg, "B", &r1, 1, &r2, 1);
	pc = fg_pass_declare(g_fg, "C", &r2, 1, NULL, 0); /* terminal */

	fg_pass_set_execute(pa, pass_a_cb, NULL);
	fg_pass_set_execute(pb, pass_b_cb, NULL);
	fg_pass_set_execute(pc, pass_c_cb, NULL);

	fg_compile(g_fg);
	renderer_null_reset_log();
	fg_execute(g_fg);

	log      = renderer_null_get_log(&count);
	draw_idx = 0;
	for (i = 0; i < count; i++) {
		if (log[i].type != GPU_CALL_CMD_DRAW_INDEXED)
			continue;
		draw_idx++;
		/* Each draw must appear in ascending sentinel order */
		assert(log[i].args.cmd_draw_indexed.index_count == draw_idx);
	}
	assert(draw_idx == 3);

	teardown();
}

/* -------------------------------------------------------------------------
 * Test: dead pass culling
 *
 * Pass D writes R3 but nothing reads R3 → D must be culled.
 * -------------------------------------------------------------------------
 */
static uint32_t g_dead_called;
static void dead_pass_cb(struct fg_pass_ctx *ctx, void *ud)
{
	(void)ctx; (void)ud;
	g_dead_called++;
}

static void test_dead_pass_culling(void)
{
	const struct gpu_call_record *log;
	fg_resource_t r1, r3;
	fg_pass_t pa, pc, pd;
	uint32_t count, i;
	fg_tex_desc td = {
		.format = GPU_FORMAT_RGBA8_UNORM,
		.width = 1, .height = 1,
		.mip_levels = 1, .sample_count = 1,
	};

	setup();
	g_dead_called = 0;

	r1 = fg_declare_transient(g_fg, "R1", td);
	r3 = fg_declare_transient(g_fg, "R3", td);

	pa = fg_pass_declare(g_fg, "A", NULL, 0, &r1, 1);
	pc = fg_pass_declare(g_fg, "C", &r1, 1, NULL, 0); /* terminal */
	pd = fg_pass_declare(g_fg, "D", NULL, 0, &r3, 1); /* dead */

	fg_pass_set_execute(pa, pass_a_cb, NULL);
	fg_pass_set_execute(pc, pass_c_cb, NULL);
	fg_pass_set_execute(pd, dead_pass_cb, NULL);

	fg_compile(g_fg);
	renderer_null_reset_log();
	fg_execute(g_fg);

	assert(g_dead_called == 0);

	/* No GPU calls from D — the only draws are index_count 1 (A) and 3 (C) */
	log = renderer_null_get_log(&count);
	for (i = 0; i < count; i++) {
		if (log[i].type == GPU_CALL_CMD_DRAW_INDEXED)
			assert(log[i].args.cmd_draw_indexed.index_count != 0);
	}

	teardown();
}

/* -------------------------------------------------------------------------
 * Test: barrier insertion
 *
 * Pass A writes R1, Pass B reads R1.
 * A CMD_BARRIER must appear between A's draw call and B's draw call.
 * -------------------------------------------------------------------------
 */
static void test_barrier_insertion(void)
{
	const struct gpu_call_record *log;
	fg_resource_t r1;
	fg_pass_t pa, pb;
	uint32_t count, i;
	int found_barrier  = 0;
	int found_a_before = 0;
	int found_b_after  = 0;
	fg_tex_desc td = {
		.format = GPU_FORMAT_RGBA8_UNORM,
		.width = 1, .height = 1,
		.mip_levels = 1, .sample_count = 1,
	};

	setup();

	r1 = fg_declare_transient(g_fg, "R1", td);

	pa = fg_pass_declare(g_fg, "A", NULL, 0, &r1, 1);
	pb = fg_pass_declare(g_fg, "B", &r1, 1, NULL, 0); /* terminal */

	fg_pass_set_execute(pa, pass_a_cb, NULL);
	fg_pass_set_execute(pb, pass_b_cb, NULL);

	fg_compile(g_fg);
	renderer_null_reset_log();
	fg_execute(g_fg);

	log = renderer_null_get_log(&count);
	for (i = 0; i < count; i++) {
		if (log[i].type == GPU_CALL_CMD_DRAW_INDEXED) {
			uint32_t ic = log[i].args.cmd_draw_indexed.index_count;

			if (ic == 1 && !found_barrier)
				found_a_before = 1;
			if (ic == 2 && found_barrier)
				found_b_after = 1;
		}
		if (log[i].type == GPU_CALL_CMD_BARRIER)
			found_barrier = 1;
	}

	assert(found_barrier);
	assert(found_a_before);
	assert(found_b_after);

	teardown();
}

/* -------------------------------------------------------------------------
 * Test: transient resource lifetime
 *
 * Pass A writes R1, Pass B reads R1.
 * TEXTURE_CREATE must precede A's draw; TEXTURE_DESTROY must follow B's draw.
 * -------------------------------------------------------------------------
 */
static void test_transient_lifetime(void)
{
	const struct gpu_call_record *log;
	fg_resource_t r1;
	fg_pass_t pa, pb;
	uint32_t count, i;
	/* phase tracks: 0=init 1=after create 2=after A draw 3=after B draw */
	int phase          = 0;
	int create_before_a = 0;
	int destroy_after_b = 0;
	fg_tex_desc td = {
		.format = GPU_FORMAT_RGBA8_UNORM,
		.width = 4, .height = 4,
		.mip_levels = 1, .sample_count = 1,
	};

	setup();

	r1 = fg_declare_transient(g_fg, "R1", td);

	pa = fg_pass_declare(g_fg, "A", NULL, 0, &r1, 1);
	pb = fg_pass_declare(g_fg, "B", &r1, 1, NULL, 0); /* terminal */

	fg_pass_set_execute(pa, pass_a_cb, NULL);
	fg_pass_set_execute(pb, pass_b_cb, NULL);

	fg_compile(g_fg);
	renderer_null_reset_log();
	fg_execute(g_fg);

	log = renderer_null_get_log(&count);
	for (i = 0; i < count; i++) {
		enum gpu_call_type t = log[i].type;

		if (t == GPU_CALL_TEXTURE_CREATE && phase == 0)
			phase = 1;

		if (t == GPU_CALL_CMD_DRAW_INDEXED) {
			uint32_t ic = log[i].args.cmd_draw_indexed.index_count;

			if (ic == 1 && phase == 1) {
				create_before_a = 1;
				phase = 2;
			}
			if (ic == 2 && phase == 2)
				phase = 3;
		}

		if (t == GPU_CALL_TEXTURE_DESTROY && phase == 3) {
			destroy_after_b = 1;
			phase = 4;
		}
	}

	assert(create_before_a);
	assert(destroy_after_b);

	teardown();
}

/* -------------------------------------------------------------------------
 * Test: render-pass setup
 *
 * The graph owns render-pass setup. Pass A writes a color target, so its
 * draw must be wrapped: BEGIN_RENDER_PASS (one color attachment) before the
 * draw, END_RENDER_PASS after it.
 * -------------------------------------------------------------------------
 */
static void test_render_pass_setup(void)
{
	const struct gpu_call_record *log;
	fg_resource_t r1;
	fg_pass_t pa, pc;
	uint32_t count, i;
	int begin_before_draw = 0;
	int end_after_draw    = 0;
	int saw_one_color     = 0;
	/* phase: 0=init 1=after begin 2=after A draw 3=after end */
	int phase = 0;
	float clear[4] = { 0.1f, 0.2f, 0.3f, 1.0f };
	fg_tex_desc td = {
		.format = GPU_FORMAT_RGBA8_UNORM,
		.width = 4, .height = 4,
		.mip_levels = 1, .sample_count = 1,
	};

	setup();

	r1 = fg_declare_transient(g_fg, "R1", td);

	pa = fg_pass_declare(g_fg, "A", NULL, 0, &r1, 1);
	pc = fg_pass_declare(g_fg, "C", &r1, 1, NULL, 0); /* terminal */

	fg_pass_set_color_clear(pa, 0, clear);
	fg_pass_set_execute(pa, pass_a_cb, NULL);
	fg_pass_set_execute(pc, pass_c_cb, NULL);

	fg_compile(g_fg);
	renderer_null_reset_log();
	fg_execute(g_fg);

	log = renderer_null_get_log(&count);
	for (i = 0; i < count; i++) {
		enum gpu_call_type t = log[i].type;

		if (t == GPU_CALL_CMD_BEGIN_RENDER_PASS) {
			if (log[i].args.cmd_begin_render_pass.color_count == 1)
				saw_one_color = 1;
			if (phase == 0)
				phase = 1;
		}
		if (t == GPU_CALL_CMD_DRAW_INDEXED &&
		    log[i].args.cmd_draw_indexed.index_count == 1 &&
		    phase == 1) {
			begin_before_draw = 1;
			phase = 2;
		}
		if (t == GPU_CALL_CMD_END_RENDER_PASS && phase == 2) {
			end_after_draw = 1;
			phase = 3;
		}
	}

	assert(saw_one_color);
	assert(begin_before_draw);
	assert(end_after_draw);

	teardown();
}

/* -------------------------------------------------------------------------
 * Test: backbuffer import
 *
 * An imported backbuffer is bound as a write target but is never
 * texture_create'd or texture_destroy'd, and the pass writing it survives
 * culling thanks to its implicit external reference.
 * -------------------------------------------------------------------------
 */
static void test_backbuffer_import(void)
{
	const struct gpu_call_record *log;
	fg_resource_t bb;
	fg_pass_t pp;
	uint32_t count, i;
	int tex_creates  = 0;
	int tex_destroys = 0;
	int saw_one_color = 0;
	int saw_draw     = 0;
	float clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

	setup();

	bb = fg_import_backbuffer(g_fg);
	assert(bb != NULL);

	pp = fg_pass_declare(g_fg, "present", NULL, 0, &bb, 1);
	fg_pass_set_color_clear(pp, 0, clear);
	fg_pass_set_execute(pp, pass_a_cb, NULL);

	fg_compile(g_fg);
	renderer_null_reset_log();
	fg_execute(g_fg);

	log = renderer_null_get_log(&count);
	for (i = 0; i < count; i++) {
		switch (log[i].type) {
		case GPU_CALL_TEXTURE_CREATE:
			tex_creates++;
			break;
		case GPU_CALL_TEXTURE_DESTROY:
			tex_destroys++;
			break;
		case GPU_CALL_CMD_BEGIN_RENDER_PASS:
			if (log[i].args.cmd_begin_render_pass.color_count == 1)
				saw_one_color = 1;
			break;
		case GPU_CALL_CMD_DRAW_INDEXED:
			saw_draw = 1;
			break;
		default:
			break;
		}
	}

	/* Imported resource is bound, never owned. */
	assert(tex_creates == 0);
	assert(tex_destroys == 0);
	/* Producer of the backbuffer was not culled. */
	assert(saw_draw);
	assert(saw_one_color);

	teardown();
}

/* -------------------------------------------------------------------------
 * Test: depth-only pass
 *
 * A pass whose only write is a depth-format transient (the sun-shadow map)
 * begins a render pass with NO color attachment (color_count == 0), and the
 * depth transient is created before the pass and destroyed after its reader —
 * exactly the shape scene_renderer's shadow pass produces.
 * -------------------------------------------------------------------------
 */
static void test_depth_only_pass(void)
{
	const struct gpu_call_record *log;
	fg_resource_t d1;
	fg_pass_t pa, pc;
	uint32_t count, i;
	int saw_zero_color   = 0;
	int saw_depth_create = 0;
	int saw_destroy      = 0;
	int saw_draw         = 0;
	fg_tex_desc td = {
		.format = GPU_FORMAT_DEPTH32_FLOAT,
		.width = 8, .height = 8,
		.mip_levels = 1, .sample_count = 1,
	};

	setup();

	d1 = fg_declare_transient(g_fg, "shadow", td);

	pa = fg_pass_declare(g_fg, "shadow", NULL, 0, &d1, 1);
	pc = fg_pass_declare(g_fg, "reader", &d1, 1, NULL, 0); /* terminal */

	fg_pass_set_depth_clear(pa, 1.0f);
	fg_pass_set_execute(pa, pass_a_cb, NULL);
	fg_pass_set_execute(pc, pass_c_cb, NULL);

	fg_compile(g_fg);
	renderer_null_reset_log();
	fg_execute(g_fg);

	log = renderer_null_get_log(&count);
	for (i = 0; i < count; i++) {
		switch (log[i].type) {
		case GPU_CALL_CMD_BEGIN_RENDER_PASS:
			if (log[i].args.cmd_begin_render_pass.color_count == 0)
				saw_zero_color = 1;
			break;
		case GPU_CALL_TEXTURE_CREATE:
			if (log[i].args.texture_create.format ==
			    GPU_FORMAT_DEPTH32_FLOAT)
				saw_depth_create = 1;
			break;
		case GPU_CALL_TEXTURE_DESTROY:
			saw_destroy = 1;
			break;
		case GPU_CALL_CMD_DRAW_INDEXED:
			saw_draw = 1;
			break;
		default:
			break;
		}
	}

	assert(saw_zero_color);   /* the shadow pass has no color attachment */
	assert(saw_depth_create); /* its depth transient was allocated */
	assert(saw_destroy);      /* and freed after the reader */
	assert(saw_draw);         /* the pass recorded its geometry */

	teardown();
}

/* -------------------------------------------------------------------------
 * Test: write-only transient is freed (regression)
 *
 * Mirrors scene_renderer's editor outline path: a forward pass writes a color
 * AND a depth target, and the composite pass reads only the color. The depth
 * transient is written but read by no pass — a pure attachment. The graph must
 * still free it: gating the free on reads alone allocated it every frame and
 * never destroyed it, leaking a viewport-sized depth texture per frame until
 * the tab OOM-crashed. Every texture the graph creates must be destroyed.
 * -------------------------------------------------------------------------
 */
static void test_write_only_transient(void)
{
	const struct gpu_call_record *log;
	fg_resource_t color, depth, bb;
	fg_resource_t fwrites[2];
	fg_pass_t fwd, comp;
	uint32_t count, i;
	int creates = 0, destroys = 0;
	int depth_created = 0;
	fg_tex_desc cdesc = {
		.format = GPU_FORMAT_RGBA8_UNORM,
		.width = 16, .height = 16,
		.mip_levels = 1, .sample_count = 1,
	};
	fg_tex_desc ddesc = cdesc;

	ddesc.format = GPU_FORMAT_DEPTH32_FLOAT;

	setup();

	color = fg_declare_transient(g_fg, "scene_color", cdesc);
	depth = fg_declare_transient(g_fg, "scene_depth", ddesc); /* written, never read */
	bb    = fg_import_backbuffer(g_fg);

	fwrites[0] = color;
	fwrites[1] = depth;
	fwd  = fg_pass_declare(g_fg, "forward", NULL, 0, fwrites, 2);
	comp = fg_pass_declare(g_fg, "composite", &color, 1, &bb, 1);

	fg_pass_set_execute(fwd, pass_a_cb, NULL);
	fg_pass_set_execute(comp, pass_b_cb, NULL);

	fg_compile(g_fg);
	renderer_null_reset_log();
	fg_execute(g_fg);

	log = renderer_null_get_log(&count);
	for (i = 0; i < count; i++) {
		if (log[i].type == GPU_CALL_TEXTURE_CREATE) {
			creates++;
			if (log[i].args.texture_create.format ==
			    GPU_FORMAT_DEPTH32_FLOAT)
				depth_created = 1;
		}
		if (log[i].type == GPU_CALL_TEXTURE_DESTROY)
			destroys++;
	}

	assert(depth_created);        /* the write-only depth transient was allocated */
	assert(creates == 2);         /* color + depth, both transients */
	assert(destroys == creates);  /* and every one was freed — no per-frame leak */

	teardown();
}

/* -------------------------------------------------------------------------
 * Test: MSAA resolve target
 *
 * Mirrors scene_renderer's MSAA path: a forward pass writes a multisampled
 * color + depth and resolves the color into a single-sample target; the
 * composite reads the RESOLVED texture, not the multisampled one. The graph
 * must (a) emit the resolve target as color[0].resolve_target on the forward
 * pass's render pass — not as a second color attachment — and (b) allocate and
 * free the resolve transient like any other produced resource, ordering the
 * reader after the pass that resolves it.
 * -------------------------------------------------------------------------
 */
static void test_resolve_target(void)
{
	const struct gpu_call_record *log;
	fg_resource_t color, depth, resolve, bb;
	fg_resource_t fwrites[2];
	fg_pass_t fwd, comp;
	uint32_t count, i;
	int creates = 0, destroys = 0;
	int fwd_has_resolve = 0, fwd_color_count = -1;
	int saw_draw = 0;
	fg_tex_desc cdesc = {
		.format = GPU_FORMAT_RGBA8_UNORM,
		.width = 16, .height = 16,
		.mip_levels = 1, .sample_count = 4,
	};
	fg_tex_desc ddesc = cdesc;
	fg_tex_desc rdesc = cdesc;

	ddesc.format = GPU_FORMAT_DEPTH32_FLOAT;
	rdesc.sample_count = 1;

	setup();

	color   = fg_declare_transient(g_fg, "scene_color", cdesc);   /* MS */
	depth   = fg_declare_transient(g_fg, "scene_depth", ddesc);   /* MS */
	resolve = fg_declare_transient(g_fg, "scene_resolve", rdesc); /* 1x */
	bb      = fg_import_backbuffer(g_fg);

	fwrites[0] = color;
	fwrites[1] = depth;
	fwd  = fg_pass_declare(g_fg, "forward", NULL, 0, fwrites, 2);
	fg_pass_set_resolve(fwd, 0, resolve);
	comp = fg_pass_declare(g_fg, "composite", &resolve, 1, &bb, 1);

	fg_pass_set_execute(fwd, pass_a_cb, NULL);
	fg_pass_set_execute(comp, pass_b_cb, NULL);

	fg_compile(g_fg);
	renderer_null_reset_log();
	fg_execute(g_fg);

	log = renderer_null_get_log(&count);
	for (i = 0; i < count; i++) {
		switch (log[i].type) {
		case GPU_CALL_TEXTURE_CREATE:
			creates++;
			break;
		case GPU_CALL_TEXTURE_DESTROY:
			destroys++;
			break;
		case GPU_CALL_CMD_BEGIN_RENDER_PASS:
			/* The forward pass is the one carrying a resolve; the
			 * composite writes the backbuffer with no resolve. */
			if (log[i].args.cmd_begin_render_pass.color0_resolve) {
				fwd_has_resolve = 1;
				fwd_color_count =
					(int)log[i].args.cmd_begin_render_pass.color_count;
			}
			break;
		case GPU_CALL_CMD_DRAW_INDEXED:
			/* index_count 2 is the composite reading the resolve. */
			if (log[i].args.cmd_draw_indexed.index_count == 2)
				saw_draw = 1;
			break;
		default:
			break;
		}
	}

	/* The resolve rode on color[0] — not as an extra color attachment. */
	assert(fwd_has_resolve);
	assert(fwd_color_count == 1);
	/* The composite ran, so the reader was scheduled after the resolve. */
	assert(saw_draw);
	/* MS color + MS depth + resolve, all allocated and all freed. */
	assert(creates == 3);
	assert(destroys == creates);

	teardown();
}

int main(void)
{
	RUN(topological_ordering);
	RUN(dead_pass_culling);
	RUN(barrier_insertion);
	RUN(transient_lifetime);
	RUN(render_pass_setup);
	RUN(backbuffer_import);
	RUN(depth_only_pass);
	RUN(write_only_transient);
	RUN(resolve_target);

	printf("%d/%d tests passed\n", tests_passed, tests_run);
	return tests_passed == tests_run ? 0 : 1;
}
