/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "renderer.h"
#include "subsystem.h"
#include "subsystem_manager.h"
#include "log_api.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/html5.h>
#include <GLES3/gl3.h>
#else
#include "log.h"
static const struct log_api native_log = { log_write };
#endif

/*
 * Internal definitions for the opaque handle types declared in renderer.h.
 * Keep all GL types as plain unsigned int so the struct compiles on native.
 */
struct gpu_cmd_buf {
	int active;
};

struct gpu_pipeline {
	unsigned int program;     /* GLuint; 0 until shaders are compiled */
	unsigned int gl_topology; /* GLenum translated from gpu_topology */
};

struct gpu_texture {
	unsigned int gl_tex; /* GLuint */
};

#ifdef __EMSCRIPTEN__
static const struct log_api           *g_log;
static EMSCRIPTEN_WEBGL_CONTEXT_HANDLE g_ctx;
static unsigned int                    g_topology; /* active draw topology */
#else
static const struct log_api *g_log = &native_log;
#endif

/* Single static sentinel — WebGL 2 is immediate-mode; no real cmd buf. */
static struct gpu_cmd_buf g_cmd_buf;

#ifdef __EMSCRIPTEN__
static unsigned int translate_topology(gpu_topology topo)
{
	switch (topo) {
	case GPU_TOPOLOGY_TRIANGLE_LIST:  return GL_TRIANGLES;
	case GPU_TOPOLOGY_TRIANGLE_STRIP: return GL_TRIANGLE_STRIP;
	case GPU_TOPOLOGY_LINE_LIST:      return GL_LINES;
	case GPU_TOPOLOGY_POINT_LIST:     return GL_POINTS;
	default:                          return GL_TRIANGLES;
	}
}
#endif

static gpu_cmd_buf_t webgl_cmd_buf_begin(void)
{
	g_cmd_buf.active = 1;
	return &g_cmd_buf;
}

static void webgl_cmd_buf_submit(gpu_cmd_buf_t cmd)
{
	(void)cmd;
	g_cmd_buf.active = 0;
}

static gpu_pipeline_t
webgl_pipeline_create(const struct gpu_pipeline_desc *desc)
{
	struct gpu_pipeline *p;

	p = malloc(sizeof(*p));
	if (!p)
		return NULL;
	/*
	 * Shader compilation is deferred; return a placeholder so callers
	 * hold a valid handle now and can submit it once shaders are ready.
	 */
	p->program = 0;
#ifdef __EMSCRIPTEN__
	p->gl_topology = translate_topology(desc->topology);
#else
	(void)desc;
	p->gl_topology = 0;
#endif
	return p;
}

static void webgl_pipeline_destroy(gpu_pipeline_t pipeline)
{
	struct gpu_pipeline *p = (struct gpu_pipeline *)pipeline;

	if (!p)
		return;
#ifdef __EMSCRIPTEN__
	if (p->program)
		glDeleteProgram(p->program);
#endif
	free(p);
}

static void webgl_cmd_set_pipeline(gpu_cmd_buf_t cmd,
				    gpu_pipeline_t pipeline)
{
	struct gpu_pipeline *p = (struct gpu_pipeline *)pipeline;

	(void)cmd;
	if (!p)
		return;
#ifdef __EMSCRIPTEN__
	if (p->program)
		glUseProgram(p->program);
	g_topology = p->gl_topology;
#else
	(void)p;
#endif
}

static void
webgl_cmd_begin_render_pass(gpu_cmd_buf_t cmd,
			     const struct gpu_render_pass_desc *desc)
{
	(void)cmd;
#ifdef __EMSCRIPTEN__
	GLbitfield clear_mask = 0;

	/* Bind default framebuffer; FBO path deferred to a later pass. */
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	if (desc->color_count > 0 &&
	    desc->color[0].load_op == GPU_LOAD_OP_CLEAR) {
		const float *c = desc->color[0].clear;

		glClearColor(c[0], c[1], c[2], c[3]);
		clear_mask |= GL_COLOR_BUFFER_BIT;
	}
	if (desc->depth && desc->depth_load_op == GPU_LOAD_OP_CLEAR) {
		glClearDepthf(desc->clear_depth);
		clear_mask |= GL_DEPTH_BUFFER_BIT;
	}
	if (clear_mask)
		glClear(clear_mask);
#else
	(void)desc;
#endif
}

static void webgl_cmd_end_render_pass(gpu_cmd_buf_t cmd)
{
	(void)cmd;
	/* WebGL 2 has no explicit pass end. */
}

static void webgl_cmd_barrier(gpu_cmd_buf_t cmd,
			       const struct gpu_barrier *barriers,
			       uint32_t count)
{
	(void)cmd;
	(void)barriers;
	(void)count;
	/* WebGL 2 synchronisation is implicit; barriers are no-ops. */
}

static void webgl_cmd_draw_indexed(gpu_cmd_buf_t cmd,
				    const struct gpu_draw_indexed_args *args,
				    void *data_gpu)
{
	(void)cmd;
	(void)data_gpu;
#ifdef __EMSCRIPTEN__
	glDrawElementsInstanced(
		g_topology,
		(GLsizei)args->index_count,
		GL_UNSIGNED_SHORT,
		(const void *)(uintptr_t)(args->first_index * 2u),
		(GLsizei)args->instance_count);
#else
	(void)args;
#endif
}

static void *webgl_gpu_malloc(size_t size)
{
	return malloc(size);
}

static void webgl_gpu_free(void *ptr)
{
	free(ptr);
}

/*
 * In WASM+WebGL 2 the WASM linear memory is directly viewable as typed
 * arrays on the JS side; CPU and GPU share the same address space.
 */
static void *webgl_gpu_host_to_device_ptr(void *host_ptr)
{
	return host_ptr;
}

static gpu_texture_t
webgl_texture_create(const struct gpu_texture_desc *desc)
{
	struct gpu_texture *t;
#ifdef __EMSCRIPTEN__
	GLuint tex_id;
#endif

	t = malloc(sizeof(*t));
	if (!t)
		return NULL;
	t->gl_tex = 0;
#ifdef __EMSCRIPTEN__
	glGenTextures(1, &tex_id);
	glBindTexture(GL_TEXTURE_2D, tex_id);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
		     (GLsizei)desc->width, (GLsizei)desc->height, 0,
		     GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glBindTexture(GL_TEXTURE_2D, 0);
	t->gl_tex = (unsigned int)tex_id;
#else
	(void)desc;
#endif
	return t;
}

static void webgl_texture_destroy(gpu_texture_t texture)
{
	struct gpu_texture *t = (struct gpu_texture *)texture;
#ifdef __EMSCRIPTEN__
	GLuint tex_id;
#endif

	if (!t)
		return;
#ifdef __EMSCRIPTEN__
	if (t->gl_tex) {
		tex_id = (GLuint)t->gl_tex;
		glDeleteTextures(1, &tex_id);
	}
#endif
	free(t);
}

static const struct gpu_api webgl_api = {
	/* WebGL 2: draw yes; compute no. */
	.caps                   = GPU_CAP_DRAW_DIRECT | GPU_CAP_DRAW_INDEXED,
	.cmd_buf_begin          = webgl_cmd_buf_begin,
	.cmd_buf_submit         = webgl_cmd_buf_submit,
	.pipeline_create        = webgl_pipeline_create,
	.pipeline_destroy       = webgl_pipeline_destroy,
	.cmd_set_pipeline       = webgl_cmd_set_pipeline,
	.cmd_begin_render_pass  = webgl_cmd_begin_render_pass,
	.cmd_end_render_pass    = webgl_cmd_end_render_pass,
	.cmd_barrier            = webgl_cmd_barrier,
	.cmd_draw_indexed       = webgl_cmd_draw_indexed,
	.cmd_dispatch           = NULL, /* no compute shaders in WebGL 2 */
	.gpu_malloc             = webgl_gpu_malloc,
	.gpu_free               = webgl_gpu_free,
	.gpu_host_to_device_ptr = webgl_gpu_host_to_device_ptr,
	.texture_create         = webgl_texture_create,
	.texture_destroy        = webgl_texture_destroy,
};

static void renderer_webgl_init(void)
{
#ifdef __EMSCRIPTEN__
	EmscriptenWebGLContextAttributes attrs;

	emscripten_webgl_init_context_attributes(&attrs);
	attrs.majorVersion = 2;
	attrs.minorVersion = 0;
	g_ctx = emscripten_webgl_create_context("#canvas", &attrs);
	emscripten_webgl_make_context_current(g_ctx);
#endif
	g_log->write(LOG_LEVEL_INFO, "renderer_webgl: init");
}

static void renderer_webgl_tick(void)
{
#ifdef __EMSCRIPTEN__
	/* Proof-of-life: clear to a solid colour each frame. */
	glClearColor(0.18f, 0.20f, 0.25f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
#endif
}

static void renderer_webgl_shutdown(void)
{
	g_log->write(LOG_LEVEL_INFO, "renderer_webgl: shutdown");
#ifdef __EMSCRIPTEN__
	emscripten_webgl_destroy_context(g_ctx);
#endif
}

static const struct subsystem desc = {
	.name     = "renderer",
	.api      = &webgl_api,
	.init     = renderer_webgl_init,
	.tick     = renderer_webgl_tick,
	.shutdown = renderer_webgl_shutdown,
};

#ifdef __EMSCRIPTEN__
void plugin_entry(struct subsystem_manager *mgr)
#else
void renderer_webgl_plugin_entry(struct subsystem_manager *mgr)
#endif
{
#ifdef __EMSCRIPTEN__
	g_log = subsystem_manager_get_api(mgr, "log");
#endif
	subsystem_manager_register(mgr, &desc);
}
