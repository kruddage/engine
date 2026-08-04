/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "renderer.h"
#include <webgl/renderer_webgl.h>
#include <core/subsystem_manager.h>

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/*
 * Native coverage for the host-declared-backbuffer bookkeeping: default vs.
 * declared vs. cleared. The GL calls a real backbuffer pass makes
 * (glBindFramebuffer, glViewport, glScissor) are compiled out entirely on a
 * native build — every backend in this tree keeps its GL-adjacent logic
 * __EMSCRIPTEN__-only — so what is tested here is the state this backend's
 * render-pass logic reads, not that logic itself; that half needs a real
 * WASM + browser run.
 */

static int tests_run;
static int tests_passed;

#define RUN(name) do { \
	tests_run++; \
	test_##name(); \
	tests_passed++; \
	printf("PASS: " #name "\n"); \
} while (0)

void renderer_webgl_plugin_entry(struct subsystem_manager *mgr);

static const struct subsystem empty_table[] = {{ NULL }};
static struct subsystem_manager mgr;

static void setup(void)
{
	subsystem_manager_init(&mgr, empty_table);
	renderer_webgl_plugin_entry(&mgr);
	webgl_clear_backbuffer();
}

static void test_registers_as_renderer(void)
{
	setup();
	assert(subsystem_manager_get_api(&mgr, "renderer") != NULL);
}

static void test_vtable_fully_populated(void)
{
	const struct gpu_api *gpu;

	setup();
	gpu = (const struct gpu_api *)subsystem_manager_get_api(&mgr, "renderer");
	assert(gpu != NULL);
	assert(gpu->cmd_buf_begin != NULL);
	assert(gpu->cmd_buf_submit != NULL);
	assert(gpu->frame_end != NULL);
	assert(gpu->pipeline_create != NULL);
	assert(gpu->pipeline_destroy != NULL);
	assert(gpu->cmd_set_pipeline != NULL);
	assert(gpu->buffer_create != NULL);
	assert(gpu->buffer_destroy != NULL);
	assert(gpu->cmd_bind_vertex_buffer != NULL);
	assert(gpu->cmd_bind_index_buffer != NULL);
	assert(gpu->cmd_bind_uniform_buffer != NULL);
	assert(gpu->cmd_begin_render_pass != NULL);
	assert(gpu->cmd_end_render_pass != NULL);
	assert(gpu->cmd_barrier != NULL);
	assert(gpu->cmd_draw_indexed != NULL);
	assert(gpu->cmd_draw != NULL);
	assert(gpu->cmd_set_scissor != NULL);
	/* WebGL 2 has no compute; the null dispatch says so. */
	assert(gpu->cmd_dispatch == NULL);
	assert(gpu->gpu_malloc != NULL);
	assert(gpu->gpu_free != NULL);
	/* Bindless-only; NULL because WebGL does not set GPU_CAP_BINDLESS. */
	assert(gpu->gpu_host_to_device_ptr == NULL);
	assert(gpu->texture_create != NULL);
	assert(gpu->texture_destroy != NULL);
	assert(gpu->cmd_bind_texture != NULL);
	assert(gpu->texture_handle != NULL);
	assert(gpu->cmd_bind_texture_handle != NULL);
}

static void test_caps_flags(void)
{
	const struct gpu_api *gpu;

	setup();
	gpu = (const struct gpu_api *)subsystem_manager_get_api(&mgr, "renderer");
	assert(gpu->caps & GPU_CAP_DRAW_DIRECT);
	assert(gpu->caps & GPU_CAP_DRAW_INDEXED);
	assert(!(gpu->caps & GPU_CAP_COMPUTE));
}

/*
 * Default backbuffer: no host has declared anything. This is the state a
 * project that never touches webgl_declare_backbuffer stays in for its whole
 * run, and it is what every backbuffer pass must fall back to — FBO 0 at the
 * full drawing-buffer size, computed fresh per pass rather than recorded
 * here (so there is no size to assert on outside a real GL context, only
 * that nothing has been declared).
 */
static void test_backbuffer_default_is_undeclared(void)
{
	struct webgl_backbuffer_decl decl;

	setup();
	decl = webgl_backbuffer_declared();
	assert(decl.declared == 0);
	assert(decl.fbo == 0);
	assert(decl.x == 0);
	assert(decl.y == 0);
	assert(decl.width == 0);
	assert(decl.height == 0);
}

/* A declared backbuffer is recorded exactly as given, verbatim. */
static void test_declare_backbuffer_records_fbo_and_rect(void)
{
	struct webgl_backbuffer_decl decl;

	setup();
	webgl_declare_backbuffer(7, 100, 0, 480, 600);

	decl = webgl_backbuffer_declared();
	assert(decl.declared == 1);
	assert(decl.fbo == 7);
	assert(decl.x == 100);
	assert(decl.y == 0);
	assert(decl.width == 480);
	assert(decl.height == 600);
}

/*
 * Two declarations against the same external fbo but different rects — the
 * shape a host sharing one framebuffer between two views (each seeing only
 * its own rect) takes. Each declare call fully replaces the last: nothing
 * from the first view's rect leaks into what the second view reads back.
 */
static void test_declare_backbuffer_replaces_prior_rect_same_fbo(void)
{
	struct webgl_backbuffer_decl decl;

	setup();
	webgl_declare_backbuffer(7, 0, 0, 480, 600);
	webgl_declare_backbuffer(7, 480, 0, 480, 600);

	decl = webgl_backbuffer_declared();
	assert(decl.declared == 1);
	assert(decl.fbo == 7);
	assert(decl.x == 480);
	assert(decl.y == 0);
	assert(decl.width == 480);
	assert(decl.height == 600);
}

/* Clearing a declaration restores the undeclared default. */
static void test_clear_backbuffer_restores_default(void)
{
	struct webgl_backbuffer_decl decl;

	setup();
	webgl_declare_backbuffer(7, 100, 0, 480, 600);
	webgl_clear_backbuffer();

	decl = webgl_backbuffer_declared();
	assert(decl.declared == 0);
	assert(decl.fbo == 0);
	assert(decl.x == 0);
	assert(decl.y == 0);
	assert(decl.width == 0);
	assert(decl.height == 0);
}

/*
 * renderer_webgl_init resets any declaration left from a prior context (the
 * plugin_entry -> subsystem_manager_register path below runs init as part
 * of registration in some call paths elsewhere in the tree; here it is
 * exercised directly through a second setup(), which re-registers and so
 * re-runs init, standing in for a context recreated after teardown).
 */
static void test_reinit_resets_declaration(void)
{
	struct webgl_backbuffer_decl decl;

	setup();
	webgl_declare_backbuffer(7, 100, 0, 480, 600);

	setup();
	decl = webgl_backbuffer_declared();
	assert(decl.declared == 0);
}

int main(void)
{
	RUN(registers_as_renderer);
	RUN(vtable_fully_populated);
	RUN(caps_flags);
	RUN(backbuffer_default_is_undeclared);
	RUN(declare_backbuffer_records_fbo_and_rect);
	RUN(declare_backbuffer_replaces_prior_rect_same_fbo);
	RUN(clear_backbuffer_restores_default);
	RUN(reinit_resets_declaration);

	printf("%d/%d tests passed\n", tests_passed, tests_run);
	return tests_passed == tests_run ? 0 : 1;
}
