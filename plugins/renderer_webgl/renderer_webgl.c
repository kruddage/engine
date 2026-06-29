/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "renderer.h"
#include "subsystem.h"
#include "subsystem_manager.h"
#include "log_api.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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

struct gpu_buffer {
	unsigned int id;     /* GLuint */
	unsigned int target; /* GLenum chosen from usage flags */
};

struct gpu_texture {
	unsigned int gl_tex; /* GLuint */
};

#ifdef __EMSCRIPTEN__
static const struct log_api           *g_log;
static EMSCRIPTEN_WEBGL_CONTEXT_HANDLE g_ctx;
static unsigned int                    g_topology;   /* active draw topology */
static unsigned int                    g_index_type = GL_UNSIGNED_SHORT;
static unsigned int                    g_index_size = 2; /* bytes per index */

/*
 * Throwaway scaffolding: a single hardcoded triangle drawn through the full
 * gpu_api each frame to prove the WebGL backend in isolation. The scene
 * renderer (#172) replaces this with entity-driven draws through the frame
 * graph.
 */
struct webgl_demo {
	int            ready;
	gpu_pipeline_t pipeline;
	gpu_buffer_t   vbo;
	gpu_buffer_t   ebo;
	gpu_buffer_t   ubo;
	unsigned int   vao; /* GLuint */
};
static struct webgl_demo g_demo;
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

static unsigned int usage_target(uint32_t usage)
{
	if (usage & GPU_BUFFER_USAGE_INDEX)
		return GL_ELEMENT_ARRAY_BUFFER;
	if (usage & GPU_BUFFER_USAGE_UNIFORM)
		return GL_UNIFORM_BUFFER;
	/* vertex and (unsupported) storage buffers bind as array buffers */
	return GL_ARRAY_BUFFER;
}

/*
 * Compile one stage from raw GLSL ES 3.00 source. Returns 0 and logs the
 * compiler's info log on failure.
 */
static GLuint compile_shader(GLenum type, const char *src)
{
	GLuint shader;
	GLint  ok = 0;

	shader = glCreateShader(type);
	glShaderSource(shader, 1, &src, NULL);
	glCompileShader(shader);
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char info[512];

		glGetShaderInfoLog(shader, (GLsizei)sizeof(info), NULL, info);
		g_log->write(LOG_LEVEL_ERROR,
			     "renderer_webgl: %s shader compile failed: %s",
			     type == GL_VERTEX_SHADER ? "vertex" : "fragment",
			     info);
		glDeleteShader(shader);
		return 0;
	}
	return shader;
}

/*
 * Compile + link the pipeline's vertex and fragment sources into a GL
 * program. Returns 0 (no program) on any failure, logging the cause.
 */
static GLuint build_program(const struct gpu_pipeline_desc *desc)
{
	GLuint vs, fs, prog;
	GLint  ok = 0;

	if (!desc->vert.src || !desc->frag.src)
		return 0;
	vs = compile_shader(GL_VERTEX_SHADER, desc->vert.src);
	if (!vs)
		return 0;
	fs = compile_shader(GL_FRAGMENT_SHADER, desc->frag.src);
	if (!fs) {
		glDeleteShader(vs);
		return 0;
	}
	prog = glCreateProgram();
	glAttachShader(prog, vs);
	glAttachShader(prog, fs);
	glLinkProgram(prog);
	glGetProgramiv(prog, GL_LINK_STATUS, &ok);
	/* The shader objects are no longer needed once linked. */
	glDeleteShader(vs);
	glDeleteShader(fs);
	if (!ok) {
		char info[512];

		glGetProgramInfoLog(prog, (GLsizei)sizeof(info), NULL, info);
		g_log->write(LOG_LEVEL_ERROR,
			     "renderer_webgl: program link failed: %s", info);
		glDeleteProgram(prog);
		return 0;
	}
	return prog;
}
#endif /* __EMSCRIPTEN__ */

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
#ifdef __EMSCRIPTEN__
	p->program     = build_program(desc);
	p->gl_topology = translate_topology(desc->topology);
#else
	(void)desc;
	p->program     = 0;
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

static gpu_buffer_t webgl_buffer_create(const struct gpu_buffer_desc *desc)
{
	struct gpu_buffer *b;
#ifdef __EMSCRIPTEN__
	GLuint id;
#endif

	b = malloc(sizeof(*b));
	if (!b)
		return NULL;
	b->id     = 0;
	b->target = 0;
#ifdef __EMSCRIPTEN__
	b->target = usage_target(desc->usage);
	glGenBuffers(1, &id);
	glBindBuffer(b->target, id);
	glBufferData(b->target, (GLsizeiptr)desc->size, desc->initial_data,
		     GL_STATIC_DRAW);
	glBindBuffer(b->target, 0);
	b->id = (unsigned int)id;
#else
	(void)desc;
#endif
	return b;
}

static void webgl_buffer_destroy(gpu_buffer_t buf)
{
	struct gpu_buffer *b = (struct gpu_buffer *)buf;
#ifdef __EMSCRIPTEN__
	GLuint id;
#endif

	if (!b)
		return;
#ifdef __EMSCRIPTEN__
	if (b->id) {
		id = (GLuint)b->id;
		glDeleteBuffers(1, &id);
	}
#endif
	free(b);
}

static void webgl_cmd_bind_vertex_buffer(gpu_cmd_buf_t cmd, uint32_t slot,
					  gpu_buffer_t buf, uint32_t offset)
{
	(void)cmd;
	(void)slot;
	(void)offset;
#ifdef __EMSCRIPTEN__
	struct gpu_buffer *b = (struct gpu_buffer *)buf;

	/*
	 * The vertex attribute layout lives in the VAO (configured once at
	 * demo setup; renderer.h has no vertex-format descriptor yet). Bind
	 * that VAO and re-attach the array buffer for completeness.
	 */
	glBindVertexArray(g_demo.vao);
	if (b)
		glBindBuffer(GL_ARRAY_BUFFER, b->id);
#else
	(void)buf;
#endif
}

static void webgl_cmd_bind_index_buffer(gpu_cmd_buf_t cmd, gpu_buffer_t buf,
					 uint32_t offset, gpu_index_format fmt)
{
	(void)cmd;
	(void)offset;
#ifdef __EMSCRIPTEN__
	struct gpu_buffer *b = (struct gpu_buffer *)buf;

	glBindVertexArray(g_demo.vao);
	if (b)
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, b->id);
	if (fmt == GPU_INDEX_FORMAT_UINT32) {
		g_index_type = GL_UNSIGNED_INT;
		g_index_size = 4;
	} else {
		g_index_type = GL_UNSIGNED_SHORT;
		g_index_size = 2;
	}
#else
	(void)buf;
	(void)fmt;
#endif
}

static void webgl_cmd_bind_uniform_buffer(gpu_cmd_buf_t cmd, uint32_t slot,
					   gpu_buffer_t buf, uint32_t offset,
					   uint32_t size)
{
	(void)cmd;
#ifdef __EMSCRIPTEN__
	struct gpu_buffer *b = (struct gpu_buffer *)buf;

	if (b)
		glBindBufferRange(GL_UNIFORM_BUFFER, slot, b->id,
				  (GLintptr)offset, (GLsizeiptr)size);
#else
	(void)slot;
	(void)buf;
	(void)offset;
	(void)size;
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
				    const struct gpu_draw_indexed_args *args)
{
	(void)cmd;
#ifdef __EMSCRIPTEN__
	glDrawElementsInstanced(
		g_topology,
		(GLsizei)args->index_count,
		g_index_type,
		(const void *)(uintptr_t)(args->first_index * g_index_size),
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
	.buffer_create          = webgl_buffer_create,
	.buffer_destroy         = webgl_buffer_destroy,
	.cmd_bind_vertex_buffer = webgl_cmd_bind_vertex_buffer,
	.cmd_bind_index_buffer  = webgl_cmd_bind_index_buffer,
	.cmd_bind_uniform_buffer = webgl_cmd_bind_uniform_buffer,
	.cmd_begin_render_pass  = webgl_cmd_begin_render_pass,
	.cmd_end_render_pass    = webgl_cmd_end_render_pass,
	.cmd_barrier            = webgl_cmd_barrier,
	.cmd_draw_indexed       = webgl_cmd_draw_indexed,
	.cmd_dispatch           = NULL, /* no compute shaders in WebGL 2 */
	.gpu_malloc             = webgl_gpu_malloc,
	.gpu_free               = webgl_gpu_free,
	/* Bindless-only; WebGL does not set GPU_CAP_BINDLESS. */
	.gpu_host_to_device_ptr = NULL,
	.texture_create         = webgl_texture_create,
	.texture_destroy        = webgl_texture_destroy,
};

#ifdef __EMSCRIPTEN__
/* GLSL ES 3.00 sources for the proof-of-life triangle. */
static const char *DEMO_VERT_SRC =
	"#version 300 es\n"
	"layout(location = 0) in vec3 a_pos;\n"
	"layout(location = 1) in vec3 a_color;\n"
	"layout(std140) uniform Globals { vec4 u_tint; };\n"
	"out vec3 v_color;\n"
	"void main() {\n"
	"	v_color = a_color * u_tint.rgb;\n"
	"	gl_Position = vec4(a_pos, 1.0);\n"
	"}\n";

static const char *DEMO_FRAG_SRC =
	"#version 300 es\n"
	"precision mediump float;\n"
	"in vec3 v_color;\n"
	"out vec4 frag_color;\n"
	"void main() {\n"
	"	frag_color = vec4(v_color, 1.0);\n"
	"}\n";

/* Interleaved position (vec3) + colour (vec3) for three vertices. */
static const float DEMO_VERTS[] = {
	 0.0f,  0.6f, 0.0f,  1.0f, 0.2f, 0.2f,
	-0.6f, -0.5f, 0.0f,  0.2f, 1.0f, 0.3f,
	 0.6f, -0.5f, 0.0f,  0.3f, 0.4f, 1.0f,
};
static const uint16_t DEMO_INDICES[] = { 0, 1, 2 };
static const float    DEMO_TINT[4]   = { 1.0f, 1.0f, 1.0f, 1.0f };

/*
 * Build the throwaway triangle's GPU resources through the gpu_api, then
 * configure a VAO with the matching attribute layout. The context must be
 * current before this runs.
 */
static void webgl_demo_setup(void)
{
	struct gpu_pipeline_desc pdesc;
	struct gpu_buffer_desc   bdesc;
	struct gpu_pipeline     *p;
	GLuint block;

	memset(&pdesc, 0, sizeof(pdesc));
	pdesc.color_formats[0]   = GPU_FORMAT_RGBA8_UNORM;
	pdesc.color_format_count = 1;
	pdesc.topology           = GPU_TOPOLOGY_TRIANGLE_LIST;
	pdesc.vert.src     = DEMO_VERT_SRC;
	pdesc.vert.stage   = GPU_SHADER_STAGE_VERTEX;
	pdesc.vert.dialect = GPU_SHADER_DIALECT_GLSL_ES_300;
	pdesc.frag.src     = DEMO_FRAG_SRC;
	pdesc.frag.stage   = GPU_SHADER_STAGE_FRAGMENT;
	pdesc.frag.dialect = GPU_SHADER_DIALECT_GLSL_ES_300;
	g_demo.pipeline = webgl_pipeline_create(&pdesc);

	memset(&bdesc, 0, sizeof(bdesc));
	bdesc.size         = sizeof(DEMO_VERTS);
	bdesc.usage        = GPU_BUFFER_USAGE_VERTEX;
	bdesc.initial_data = DEMO_VERTS;
	g_demo.vbo = webgl_buffer_create(&bdesc);

	bdesc.size         = sizeof(DEMO_INDICES);
	bdesc.usage        = GPU_BUFFER_USAGE_INDEX;
	bdesc.initial_data = DEMO_INDICES;
	g_demo.ebo = webgl_buffer_create(&bdesc);

	bdesc.size         = sizeof(DEMO_TINT);
	bdesc.usage        = GPU_BUFFER_USAGE_UNIFORM;
	bdesc.initial_data = DEMO_TINT;
	g_demo.ubo = webgl_buffer_create(&bdesc);

	/*
	 * Capture the attribute layout into a VAO: location 0 = vec3 position,
	 * location 1 = vec3 colour, interleaved with a 6-float stride. The
	 * element buffer binding is captured by the VAO too.
	 */
	glGenVertexArrays(1, &g_demo.vao);
	glBindVertexArray(g_demo.vao);
	glBindBuffer(GL_ARRAY_BUFFER, ((struct gpu_buffer *)g_demo.vbo)->id);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
			      6 * (GLsizei)sizeof(float), (const void *)0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE,
			      6 * (GLsizei)sizeof(float),
			      (const void *)(3 * sizeof(float)));
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,
		     ((struct gpu_buffer *)g_demo.ebo)->id);
	glBindVertexArray(0);

	/* Wire the shader's Globals uniform block to binding point 0. */
	p = (struct gpu_pipeline *)g_demo.pipeline;
	if (p && p->program) {
		block = glGetUniformBlockIndex(p->program, "Globals");
		if (block != GL_INVALID_INDEX)
			glUniformBlockBinding(p->program, block, 0);
	}

	g_demo.ready = 1;
	g_log->write(LOG_LEVEL_INFO, "renderer_webgl: triangle demo ready");
}

static void webgl_demo_teardown(void)
{
	if (!g_demo.ready)
		return;
	glDeleteVertexArrays(1, &g_demo.vao);
	webgl_buffer_destroy(g_demo.ubo);
	webgl_buffer_destroy(g_demo.ebo);
	webgl_buffer_destroy(g_demo.vbo);
	webgl_pipeline_destroy(g_demo.pipeline);
	g_demo.ready = 0;
}
#endif /* __EMSCRIPTEN__ */

static void renderer_webgl_init(void)
{
#ifdef __EMSCRIPTEN__
	EmscriptenWebGLContextAttributes attrs;

	emscripten_webgl_init_context_attributes(&attrs);
	attrs.majorVersion = 2;
	attrs.minorVersion = 0;
	g_ctx = emscripten_webgl_create_context("#canvas", &attrs);
	emscripten_webgl_make_context_current(g_ctx);
	webgl_demo_setup();
#endif
	g_log->write(LOG_LEVEL_INFO, "renderer_webgl: init");
}

static void renderer_webgl_tick(void)
{
#ifdef __EMSCRIPTEN__
	struct gpu_render_pass_desc  pass;
	struct gpu_draw_indexed_args draw;
	gpu_cmd_buf_t                cmd;

	if (!g_demo.ready)
		return;

	memset(&pass, 0, sizeof(pass));
	pass.color_count       = 1;
	pass.color[0].load_op  = GPU_LOAD_OP_CLEAR;
	pass.color[0].store_op = GPU_STORE_OP_STORE;
	pass.color[0].clear[0] = 0.18f;
	pass.color[0].clear[1] = 0.20f;
	pass.color[0].clear[2] = 0.25f;
	pass.color[0].clear[3] = 1.0f;

	memset(&draw, 0, sizeof(draw));
	draw.index_count    = 3;
	draw.instance_count = 1;

	/* Drive the hardcoded triangle entirely through the gpu_api. */
	cmd = webgl_cmd_buf_begin();
	webgl_cmd_begin_render_pass(cmd, &pass);
	webgl_cmd_set_pipeline(cmd, g_demo.pipeline);
	webgl_cmd_bind_uniform_buffer(cmd, 0, g_demo.ubo, 0,
				      (uint32_t)sizeof(DEMO_TINT));
	webgl_cmd_bind_vertex_buffer(cmd, 0, g_demo.vbo, 0);
	webgl_cmd_bind_index_buffer(cmd, g_demo.ebo, 0, GPU_INDEX_FORMAT_UINT16);
	webgl_cmd_draw_indexed(cmd, &draw);
	webgl_cmd_end_render_pass(cmd);
	webgl_cmd_buf_submit(cmd);
#endif
}

static void renderer_webgl_shutdown(void)
{
	g_log->write(LOG_LEVEL_INFO, "renderer_webgl: shutdown");
#ifdef __EMSCRIPTEN__
	webgl_demo_teardown();
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
