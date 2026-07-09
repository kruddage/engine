/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "renderer.h"
#include "subsystem.h"
#include "subsystem_manager.h"
#include "log_api.h"
#include "memory_api.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/html5.h>
#include <GLES3/gl3.h>
#include "script.h"
#else
#include "log.h"
#include "memory.h"
static const struct log_api    native_log = { log_write };
static const struct memory_api native_mem = {
	mem_alloc, mem_alloc_zero, mem_free,
	mem_pool_create, mem_pool_alloc, mem_pool_free, mem_pool_destroy,
};
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
	unsigned int vao;         /* GLuint; attribute layout captured here */
	struct gpu_vertex_layout layout;
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
static const struct memory_api        *g_mem;
static EMSCRIPTEN_WEBGL_CONTEXT_HANDLE g_ctx;
static unsigned int                    g_topology;   /* active draw topology */
static unsigned int                    g_index_type = GL_UNSIGNED_SHORT;
static unsigned int                    g_index_size = 2; /* bytes per index */
static struct gpu_pipeline            *g_cur_pipeline; /* bound by set_pipeline */
#else
static const struct log_api    *g_log = &native_log;
static const struct memory_api *g_mem = &native_mem;
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

/* Float components in a vertex-attribute format (0 if not a float vector). */
static int format_components(gpu_format fmt)
{
	switch (fmt) {
	case GPU_FORMAT_RG32_FLOAT:   return 2;
	case GPU_FORMAT_RGB32_FLOAT:  return 3;
	case GPU_FORMAT_RGBA32_FLOAT: return 4;
	default:                      return 0;
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
 * Compile one stage of a pipeline. A glsl_es_300 source compiles as-is; a
 * krudd (DSL) source is lowered to GLSL first through the Scheme transpiler in
 * the runtime image — the backend owns turning the portable DSL into what this
 * GPU speaks. Returns 0 and logs on a transpile miss or a compiler error.
 */
static GLuint compile_shader(GLenum type, const struct gpu_shader_source *s)
{
	const char *src   = s->src;
	const char *label = type == GL_VERTEX_SHADER ? "vertex" : "fragment";
	GLuint      shader;
	GLint       ok = 0;

	if (s->dialect == GPU_SHADER_DIALECT_KRUDD) {
		src = script_shader_transpile(s->src, label);
		if (!src) {
			g_log->write(LOG_LEVEL_ERROR,
				     "renderer_webgl: %s shader transpile failed",
				     label);
			return 0;
		}
	}

	shader = glCreateShader(type);
	glShaderSource(shader, 1, &src, NULL);
	glCompileShader(shader);
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char info[512];

		glGetShaderInfoLog(shader, (GLsizei)sizeof(info), NULL, info);
		g_log->write(LOG_LEVEL_ERROR,
			     "renderer_webgl: %s shader compile failed: %s",
			     label, info);
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
	vs = compile_shader(GL_VERTEX_SHADER, &desc->vert);
	if (!vs)
		return 0;
	fs = compile_shader(GL_FRAGMENT_SHADER, &desc->frag);
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

	p = g_mem->alloc(sizeof(*p));
	if (!p)
		return NULL;
	p->layout = desc->vertex_layout;
#ifdef __EMSCRIPTEN__
	p->program     = build_program(desc);
	p->gl_topology = translate_topology(desc->topology);
	/*
	 * Bind each uniform block to the binding point matching its index, so a
	 * consumer with no GL access can bind its UBO to cmd_bind_uniform_buffer
	 * slot == block index (the sole block lands at slot 0).
	 */
	if (p->program) {
		GLint nblocks = 0, bi;

		glGetProgramiv(p->program, GL_ACTIVE_UNIFORM_BLOCKS, &nblocks);
		for (bi = 0; bi < nblocks; bi++)
			glUniformBlockBinding(p->program, (GLuint)bi, (GLuint)bi);
	}
	/*
	 * A VAO owns the pipeline's attribute state. WebGL 2 (GLES 3.0) has no
	 * separate attrib-format API, so the attribute pointers are (re)specified
	 * against the mesh's vertex buffer at bind time (see bind_vertex_buffer);
	 * the VAO just holds the enabled attributes and index-buffer binding.
	 */
	glGenVertexArrays(1, &p->vao);
#else
	p->program     = 0;
	p->gl_topology = 0;
	p->vao         = 0;
#endif
	return p;
}

static void webgl_pipeline_destroy(gpu_pipeline_t pipeline)
{
	struct gpu_pipeline *p = (struct gpu_pipeline *)pipeline;

	if (!p)
		return;
#ifdef __EMSCRIPTEN__
	if (p->vao) {
		if (g_cur_pipeline == p)
			g_cur_pipeline = NULL;
		glDeleteVertexArrays(1, &p->vao);
	}
	if (p->program)
		glDeleteProgram(p->program);
#endif
	g_mem->free(p);
}

static void webgl_cmd_set_pipeline(gpu_cmd_buf_t cmd,
				    gpu_pipeline_t pipeline)
{
	struct gpu_pipeline *p = (struct gpu_pipeline *)pipeline;

	(void)cmd;
	if (!p)
		return;
#ifdef __EMSCRIPTEN__
	g_cur_pipeline = p;
	if (p->vao)
		glBindVertexArray(p->vao);
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

	b = g_mem->alloc(sizeof(*b));
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
	g_mem->free(b);
}

static void webgl_buffer_update(gpu_buffer_t buf, uint32_t offset,
				 const void *data, uint32_t size)
{
	struct gpu_buffer *b = (struct gpu_buffer *)buf;

	if (!b)
		return;
#ifdef __EMSCRIPTEN__
	glBindBuffer(b->target, b->id);
	glBufferSubData(b->target, (GLintptr)offset, (GLsizeiptr)size, data);
	glBindBuffer(b->target, 0);
#else
	(void)offset;
	(void)data;
	(void)size;
#endif
}

static void webgl_cmd_bind_vertex_buffer(gpu_cmd_buf_t cmd, uint32_t slot,
					  gpu_buffer_t buf, uint32_t offset)
{
	(void)cmd;
	(void)slot;
#ifdef __EMSCRIPTEN__
	struct gpu_buffer *b = (struct gpu_buffer *)buf;
	uint32_t           i;

	if (!b || !g_cur_pipeline)
		return;
	/*
	 * WebGL 2 captures the array-buffer binding into glVertexAttribPointer at
	 * call time, so re-specify the current pipeline's attribute pointers
	 * against this mesh's vertex buffer. slot 0 is the only stream.
	 */
	glBindVertexArray(g_cur_pipeline->vao);
	glBindBuffer(GL_ARRAY_BUFFER, b->id);
	for (i = 0; i < g_cur_pipeline->layout.attr_count; i++) {
		const struct gpu_vertex_attr *a =
			&g_cur_pipeline->layout.attrs[i];
		int comps = format_components(a->format);

		if (comps == 0)
			continue;
		glEnableVertexAttribArray(a->location);
		glVertexAttribPointer(a->location, comps, GL_FLOAT, GL_FALSE,
				      (GLsizei)g_cur_pipeline->layout.stride,
				      (const void *)(uintptr_t)(offset + a->offset));
	}
#else
	(void)buf;
	(void)offset;
#endif
}

static void webgl_cmd_bind_index_buffer(gpu_cmd_buf_t cmd, gpu_buffer_t buf,
					 uint32_t offset, gpu_index_format fmt)
{
	(void)cmd;
	(void)offset;
#ifdef __EMSCRIPTEN__
	struct gpu_buffer *b = (struct gpu_buffer *)buf;

	if (g_cur_pipeline)
		glBindVertexArray(g_cur_pipeline->vao);
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
	int        dw = 0, dh = 0;

	/* Bind default framebuffer; FBO path deferred to a later pass. */
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	/*
	 * Establish our own GL state rather than inheriting whatever the
	 * previous subsystem (the ImGui backend) left behind. ImGui leaves
	 * GL_SCISSOR_TEST and GL_BLEND enabled with a stale scissor box, which
	 * otherwise clips the clear and the draw to a corner of the canvas.
	 */
	emscripten_webgl_get_drawing_buffer_size(g_ctx, &dw, &dh);
	glViewport(0, 0, dw, dh);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_CULL_FACE);
	/*
	 * Depth test against the canvas's own depth buffer. The frame graph
	 * scopes offscreen depth targets out for now, so 3D passes rendering to
	 * the backbuffer rely on this default-framebuffer depth. glDepthMask
	 * must be enabled for the depth clear below to take effect.
	 */
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);
	glDepthMask(GL_TRUE);

	if (desc->color_count > 0 &&
	    desc->color[0].load_op == GPU_LOAD_OP_CLEAR) {
		const float *c = desc->color[0].clear;

		glClearColor(c[0], c[1], c[2], c[3]);
		clear_mask |= GL_COLOR_BUFFER_BIT;
	}
	/*
	 * Clear the backbuffer depth each pass. Honour an explicit clear value
	 * when the pass supplies one, else reset to the far plane (1.0).
	 */
	glClearDepthf(desc->depth_load_op == GPU_LOAD_OP_CLEAR
		      ? desc->clear_depth : 1.0f);
	clear_mask |= GL_DEPTH_BUFFER_BIT;
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
	return g_mem->alloc(size);
}

static void webgl_gpu_free(void *ptr)
{
	g_mem->free(ptr);
}

static gpu_texture_t
webgl_texture_create(const struct gpu_texture_desc *desc)
{
	struct gpu_texture *t;
#ifdef __EMSCRIPTEN__
	GLuint tex_id;
#endif

	t = g_mem->alloc(sizeof(*t));
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
	g_mem->free(t);
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
	.buffer_update          = webgl_buffer_update,
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

static void renderer_webgl_init(void)
{
#ifdef __EMSCRIPTEN__
	EmscriptenWebGLContextAttributes attrs;

	emscripten_webgl_init_context_attributes(&attrs);
	attrs.majorVersion = 2;
	attrs.minorVersion = 0;
	attrs.depth        = EM_TRUE; /* backbuffer depth for 3D passes */
	g_ctx = emscripten_webgl_create_context("#canvas", &attrs);
	emscripten_webgl_make_context_current(g_ctx);
#endif
	g_log->write(LOG_LEVEL_INFO, "renderer_webgl: init");
}

/*
 * The renderer subsystem no longer drives per-frame drawing: the scene renderer
 * (#172) records draws through the frame graph on the "renderer" device. This
 * backend just owns the GL context and the gpu_api vtable, so there is no tick.
 */

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
	.shutdown = renderer_webgl_shutdown,
};

void renderer_webgl_plugin_entry(struct subsystem_manager *mgr)
{
#ifdef __EMSCRIPTEN__
	g_log = subsystem_manager_get_api(mgr, "log");
	g_mem = subsystem_manager_get_api(mgr, "memory");
#endif
	subsystem_manager_register(mgr, &desc);
}
