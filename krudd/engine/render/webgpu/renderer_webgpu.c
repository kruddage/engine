/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * WebGPU renderer backend — the gpu_api vtable.
 *
 * This is the second slice. The first stood up the emdawnwebgpu toolchain and
 * drew a triangle with hand-rolled wgpu calls; this one turns that into a real
 * gpu_api provider registered as the "renderer" subsystem, the same seam the
 * WebGL backend fills. The triangle survives as the test load: it is now built
 * and drawn entirely through the vtable, so it exercises pipeline_create,
 * buffer_create, the render pass, and the draw path end to end. Nothing on
 * screen should change — this slice is a refactor with a visible invariant.
 *
 * What is real here: command buffers, buffers, pipelines (through the krudd
 * shader DSL lowered to WGSL), vertex/index binding, backbuffer render passes,
 * and both draw entry points. What is stubbed: textures, uniform buffers,
 * scissor, barriers, and compute. Stubs are loud rather than absent — every
 * vtable entry is non-NULL and logs once on first use, so a caller reaching for
 * an unimplemented entry says so in the console instead of hitting a NULL
 * function pointer. Later slices replace stubs in place.
 *
 * It is browser-only. Selection lives in engine.c: with ?renderer=webgpu the
 * engine registers this backend alone and skips the GL render cluster. The
 * device handshake is async (adapter -> device callbacks); the tick no-ops
 * until the device is ready.
 */
#include "subsystem.h"
#include "subsystem_manager.h"

#ifdef __EMSCRIPTEN__
#include "renderer.h"
#include "log_api.h"
#include "memory_api.h"
#include "script.h"

#include <emscripten.h>
#include <emscripten/html5.h>
#include <webgpu/webgpu.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const struct log_api    *g_log;
static const struct memory_api *g_mem;

static WGPUInstance      g_instance;
static WGPUSurface       g_surface;
static WGPUAdapter       g_adapter;
static WGPUDevice        g_device;
static WGPUQueue         g_queue;
static WGPUTextureFormat g_format;
static int               g_ready;    /* 1 once the device + surface are live */

/* The triangle, now owned as ordinary gpu_api resources. */
static gpu_pipeline_t g_tri_pso;
static gpu_buffer_t   g_tri_vbuf;

/*
 * The triangle authored in the krudd shader DSL — the same source of truth the
 * WebGL path uses, lowered to WGSL through the runtime (shader-transpile-wgsl)
 * so this exercises the real shader pipeline, not a hand-written WGSL string.
 */
static const char *TRI_SHADER =
	"(shader tri"
	"  (inputs (a_pos vec2 (location 0)) (a_col vec3 (location 1)))"
	"  (varyings (v_col vec3))"
	"  (targets (frag_color vec4 (location 0)))"
	"  (vertex (set v_col a_col) (set position (vec4 a_pos 0.0 1.0)))"
	"  (fragment (set frag_color (vec4 v_col 1.0))))";

/* 3 vertices, interleaved: pos.x pos.y  col.r col.g col.b (stride 20 bytes). */
static const float TRI_VERTS[] = {
	 0.0f,  0.6f,   1.0f, 0.25f, 0.35f,
	-0.6f, -0.5f,   0.25f, 1.0f, 0.4f,
	 0.6f, -0.5f,   0.3f, 0.45f, 1.0f,
};

/*
 * Tell the shell WebGPU went live so the header badge flips (kruddSetRenderer
 * from shell.html). Named distinctly from the WebGL backend's reporter — both
 * are compiled into the one WASM module, so a shared symbol would collide.
 */
EM_JS(void, webgpu_announce_renderer, (void), {
	if (typeof window.kruddSetRenderer === 'function')
		window.kruddSetRenderer('webgpu');
})

/*
 * Surface progress into the shell's scrolling WebGPU log panel (and the
 * console), so a browser without a working adapter/device reports where it
 * stopped, with full history, instead of just a blank canvas. Diagnostic
 * scaffolding for the port; the kruddgui editor console takes over once WebGPU
 * can drive the UI.
 */
EM_JS(void, webgpu_status, (const char *msg), {
	var s = UTF8ToString(msg);
	if (typeof window.kruddWebGPULog === 'function')
		window.kruddWebGPULog(s);
	if (typeof console !== 'undefined')
		console.log('[webgpu] ' + s);
})

/* A WGPUStringView over a NUL-terminated C string (the emdawnwebgpu string ABI). */
static WGPUStringView str_view(const char *s)
{
	WGPUStringView v;

	v.data   = s;
	v.length = strlen(s);
	return v;
}

/*
 * Report an unimplemented vtable entry once per entry. A later slice fills
 * these in; until then a caller that reaches one gets a line in the console
 * naming what is missing, rather than silence or a crash.
 */
#define STUB_ONCE(name)                                                        \
	do {                                                                   \
		static int reported;                                           \
		if (!reported) {                                               \
			reported = 1;                                          \
			g_log->write(LOG_LEVEL_WARN,                           \
				     "renderer_webgpu: " name                  \
				     " not implemented yet");                  \
		}                                                              \
	} while (0)

/* ------------------------------------------------------------ handles */

struct gpu_buffer {
	WGPUBuffer buf;
	uint32_t   size;
};

struct gpu_pipeline {
	WGPURenderPipeline pso;
};

/*
 * One command buffer in flight, matching how the engine actually draws: a
 * caller does cmd_buf_begin -> passes -> cmd_buf_submit before the next
 * begin. The WebGL backend makes the same assumption implicitly (it ignores
 * its cmd handle entirely and drives global GL state); this makes it explicit.
 */
struct gpu_cmd_buf_state {
	WGPUCommandEncoder    enc;
	WGPURenderPassEncoder pass;
	/* Backbuffer texture acquired for this submission, if a pass asked for
	 * it. Held until submit so the view outlives the pass that draws into it. */
	WGPUTexture     surface_tex;
	WGPUTextureView surface_view;
	int             in_use;
};

static struct gpu_cmd_buf_state g_cmd;

/* ------------------------------------------------- format translation */

static WGPUTextureFormat to_texture_format(gpu_format f)
{
	switch (f) {
	case GPU_FORMAT_RGBA8_UNORM:   return WGPUTextureFormat_RGBA8Unorm;
	case GPU_FORMAT_BGRA8_UNORM:   return WGPUTextureFormat_BGRA8Unorm;
	case GPU_FORMAT_DEPTH32_FLOAT: return WGPUTextureFormat_Depth32Float;
	case GPU_FORMAT_RGBA32_FLOAT:  return WGPUTextureFormat_RGBA32Float;
	default:                       return WGPUTextureFormat_Undefined;
	}
}

/* Vertex attribute formats. The DSL's vec2/vec3/vec4 arrive as the rg/rgb/rgba
 * 32-float triple; anything else has no vertex meaning. */
static WGPUVertexFormat to_vertex_format(gpu_format f)
{
	switch (f) {
	case GPU_FORMAT_RG32_FLOAT:   return WGPUVertexFormat_Float32x2;
	case GPU_FORMAT_RGB32_FLOAT:  return WGPUVertexFormat_Float32x3;
	case GPU_FORMAT_RGBA32_FLOAT: return WGPUVertexFormat_Float32x4;
	default:                      return WGPUVertexFormat_Float32;
	}
}

static WGPUPrimitiveTopology to_topology(gpu_topology t)
{
	switch (t) {
	case GPU_TOPOLOGY_TRIANGLE_STRIP: return WGPUPrimitiveTopology_TriangleStrip;
	case GPU_TOPOLOGY_LINE_LIST:      return WGPUPrimitiveTopology_LineList;
	case GPU_TOPOLOGY_POINT_LIST:     return WGPUPrimitiveTopology_PointList;
	default:                          return WGPUPrimitiveTopology_TriangleList;
	}
}

static WGPUIndexFormat to_index_format(gpu_index_format f)
{
	return (f == GPU_INDEX_FORMAT_UINT32) ? WGPUIndexFormat_Uint32
					      : WGPUIndexFormat_Uint16;
}

/* WebGPU has no dont-care load: clearing is the honest cheap option. */
static WGPULoadOp to_load_op(gpu_load_op op)
{
	return (op == GPU_LOAD_OP_LOAD) ? WGPULoadOp_Load : WGPULoadOp_Clear;
}

static WGPUStoreOp to_store_op(gpu_store_op op)
{
	return (op == GPU_STORE_OP_DONT_CARE) ? WGPUStoreOp_Discard
					      : WGPUStoreOp_Store;
}

/* ------------------------------------------------------ command buffers */

static gpu_cmd_buf_t webgpu_cmd_buf_begin(void)
{
	if (!g_ready)
		return NULL;
	if (g_cmd.in_use) {
		g_log->write(LOG_LEVEL_ERROR,
			     "renderer_webgpu: cmd_buf_begin while one is already open");
		return NULL;
	}

	memset(&g_cmd, 0, sizeof(g_cmd));
	g_cmd.enc    = wgpuDeviceCreateCommandEncoder(g_device, NULL);
	g_cmd.in_use = 1;
	return (gpu_cmd_buf_t)&g_cmd;
}

static void webgpu_cmd_buf_submit(gpu_cmd_buf_t cmd)
{
	struct gpu_cmd_buf_state *c = (struct gpu_cmd_buf_state *)cmd;
	WGPUCommandBuffer buf;

	if (!c || !c->in_use)
		return;

	buf = wgpuCommandEncoderFinish(c->enc, NULL);
	wgpuQueueSubmit(g_queue, 1, &buf);
	wgpuCommandBufferRelease(buf);
	wgpuCommandEncoderRelease(c->enc);

	if (c->surface_view)
		wgpuTextureViewRelease(c->surface_view);
	if (c->surface_tex)
		wgpuTextureRelease(c->surface_tex);

	memset(c, 0, sizeof(*c));
}

/* ------------------------------------------------------------ buffers */

static WGPUBufferUsage to_buffer_usage(uint32_t usage)
{
	WGPUBufferUsage u = WGPUBufferUsage_CopyDst;

	if (usage & GPU_BUFFER_USAGE_VERTEX)
		u |= WGPUBufferUsage_Vertex;
	if (usage & GPU_BUFFER_USAGE_INDEX)
		u |= WGPUBufferUsage_Index;
	if (usage & GPU_BUFFER_USAGE_UNIFORM)
		u |= WGPUBufferUsage_Uniform;
	if (usage & GPU_BUFFER_USAGE_STORAGE)
		u |= WGPUBufferUsage_Storage;
	return u;
}

static gpu_buffer_t webgpu_buffer_create(const struct gpu_buffer_desc *desc)
{
	struct gpu_buffer *b;
	WGPUBufferDescriptor bd;
	size_t size;

	if (!g_ready)
		return NULL;

	b = g_mem->alloc(sizeof(*b));
	if (!b)
		return NULL;

	/*
	 * WebGPU requires a buffer size that is a multiple of 4; GL did not, so
	 * a caller that sized a buffer exactly is not wrong, it is just writing
	 * for the looser API. Round up rather than reject.
	 */
	size = (desc->size + 3u) & ~(size_t)3u;

	memset(&bd, 0, sizeof(bd));
	bd.usage = to_buffer_usage(desc->usage);
	bd.size  = size;

	b->buf  = wgpuDeviceCreateBuffer(g_device, &bd);
	b->size = (uint32_t)size;

	if (!b->buf) {
		g_mem->free(b);
		return NULL;
	}
	if (desc->initial_data)
		wgpuQueueWriteBuffer(g_queue, b->buf, 0, desc->initial_data,
				     desc->size);
	return (gpu_buffer_t)b;
}

static void webgpu_buffer_destroy(gpu_buffer_t buf)
{
	struct gpu_buffer *b = (struct gpu_buffer *)buf;

	if (!b)
		return;
	if (b->buf) {
		wgpuBufferDestroy(b->buf);
		wgpuBufferRelease(b->buf);
	}
	g_mem->free(b);
}

static void webgpu_buffer_update(gpu_buffer_t buf, uint32_t offset,
				 const void *data, uint32_t size)
{
	struct gpu_buffer *b = (struct gpu_buffer *)buf;

	if (!b || !b->buf || !data || !size)
		return;
	wgpuQueueWriteBuffer(g_queue, b->buf, offset, data, size);
}

/* ----------------------------------------------------------- pipelines */

/* Compile one WGSL stage string into a shader module. */
static WGPUShaderModule make_module(const char *wgsl)
{
	WGPUShaderSourceWGSL src;
	WGPUShaderModuleDescriptor desc;

	memset(&src, 0, sizeof(src));
	src.chain.sType = WGPUSType_ShaderSourceWGSL;
	src.code = str_view(wgsl);
	memset(&desc, 0, sizeof(desc));
	desc.nextInChain = &src.chain;
	return wgpuDeviceCreateShaderModule(g_device, &desc);
}

/*
 * Lower one stage of a gpu_shader_source to WGSL. The krudd DSL goes through
 * the runtime transpiler; GLSL cannot be consumed by WebGPU at all, so a
 * GLSL-dialect pipeline is refused rather than silently producing nothing.
 */
static const char *lower_stage(const struct gpu_shader_source *src,
			       const char *stage_name)
{
	if (!src->src)
		return NULL;
	if (src->dialect != GPU_SHADER_DIALECT_KRUDD) {
		g_log->write(LOG_LEVEL_ERROR,
			     "renderer_webgpu: %s stage is GLSL; WebGPU needs the krudd dialect",
			     stage_name);
		return NULL;
	}
	return script_shader_transpile_wgsl(src->src, stage_name);
}

static gpu_pipeline_t webgpu_pipeline_create(const struct gpu_pipeline_desc *desc)
{
	struct gpu_pipeline *p;
	const char *vs_wgsl;
	const char *fs_wgsl;
	WGPUShaderModule vmod = NULL;
	WGPUShaderModule fmod = NULL;
	WGPUVertexAttribute attrs[GPU_MAX_VERTEX_ATTRS];
	WGPUVertexBufferLayout vblayout;
	WGPUColorTargetState targets[GPU_MAX_COLOR_ATTACHMENTS];
	WGPUBlendState blend;
	WGPUFragmentState frag;
	WGPUDepthStencilState depth;
	WGPURenderPipelineDescriptor pd;
	uint32_t i;

	if (!g_ready)
		return NULL;

	vs_wgsl = lower_stage(&desc->vert, "vertex");
	fs_wgsl = lower_stage(&desc->frag, "fragment");
	if (!vs_wgsl || !fs_wgsl) {
		g_log->write(LOG_LEVEL_ERROR,
			     "renderer_webgpu: pipeline_create shader lowering failed");
		return NULL;
	}

	p = g_mem->alloc(sizeof(*p));
	if (!p)
		return NULL;

	vmod = make_module(vs_wgsl);
	fmod = make_module(fs_wgsl);

	memset(attrs, 0, sizeof(attrs));
	for (i = 0; i < desc->vertex_layout.attr_count && i < GPU_MAX_VERTEX_ATTRS; i++) {
		const struct gpu_vertex_attr *a = &desc->vertex_layout.attrs[i];

		attrs[i].format         = to_vertex_format(a->format);
		attrs[i].offset         = a->offset;
		attrs[i].shaderLocation = a->location;
	}

	memset(&vblayout, 0, sizeof(vblayout));
	vblayout.arrayStride    = desc->vertex_layout.stride;
	vblayout.stepMode       = WGPUVertexStepMode_Vertex;
	vblayout.attributeCount = desc->vertex_layout.attr_count;
	vblayout.attributes     = attrs;

	memset(targets, 0, sizeof(targets));
	memset(&blend, 0, sizeof(blend));
	if (desc->blend_enable) {
		/* Straight-alpha compositing, matching the GL backend's 2D overlay. */
		blend.color.operation = WGPUBlendOperation_Add;
		blend.color.srcFactor = WGPUBlendFactor_SrcAlpha;
		blend.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
		blend.alpha.operation = WGPUBlendOperation_Add;
		blend.alpha.srcFactor = WGPUBlendFactor_One;
		blend.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
	}
	for (i = 0; i < desc->color_format_count && i < GPU_MAX_COLOR_ATTACHMENTS; i++) {
		targets[i].format    = to_texture_format(desc->color_formats[i]);
		targets[i].writeMask = WGPUColorWriteMask_All;
		if (desc->blend_enable)
			targets[i].blend = &blend;
	}
	/*
	 * A pipeline that names no color format still draws to the backbuffer —
	 * that is how the probe and any pass targeting the surface behave. Fall
	 * back to the surface format rather than creating a target-less pipeline.
	 */
	if (desc->color_format_count == 0) {
		targets[0].format    = g_format;
		targets[0].writeMask = WGPUColorWriteMask_All;
		if (desc->blend_enable)
			targets[0].blend = &blend;
	}

	memset(&frag, 0, sizeof(frag));
	frag.module      = fmod;
	frag.entryPoint  = str_view("fs_main");
	frag.targetCount = desc->color_format_count ? desc->color_format_count : 1;
	frag.targets     = targets;

	memset(&depth, 0, sizeof(depth));
	memset(&pd, 0, sizeof(pd));
	pd.layout                     = NULL; /* auto layout until bind groups land */
	pd.vertex.module              = vmod;
	pd.vertex.entryPoint          = str_view("vs_main");
	pd.vertex.bufferCount         = desc->vertex_layout.attr_count ? 1 : 0;
	pd.vertex.buffers             = desc->vertex_layout.attr_count ? &vblayout : NULL;
	pd.primitive.topology         = to_topology(desc->topology);
	pd.primitive.stripIndexFormat =
		(desc->topology == GPU_TOPOLOGY_TRIANGLE_STRIP)
			? to_index_format(desc->strip_index_format)
			: WGPUIndexFormat_Undefined;
	pd.multisample.count = desc->sample_count ? desc->sample_count : 1;
	pd.multisample.mask  = 0xFFFFFFFFu;
	pd.fragment          = &frag;

	if (desc->depth_format != GPU_FORMAT_UNKNOWN) {
		depth.format            = to_texture_format(desc->depth_format);
		depth.depthWriteEnabled = desc->disable_depth_test
						  ? WGPUOptionalBool_False
						  : WGPUOptionalBool_True;
		depth.depthCompare      = desc->disable_depth_test
						  ? WGPUCompareFunction_Always
						  : WGPUCompareFunction_Less;
		pd.depthStencil         = &depth;
	}

	p->pso = wgpuDeviceCreateRenderPipeline(g_device, &pd);

	wgpuShaderModuleRelease(vmod);
	wgpuShaderModuleRelease(fmod);

	if (!p->pso) {
		g_log->write(LOG_LEVEL_ERROR,
			     "renderer_webgpu: render pipeline creation failed");
		g_mem->free(p);
		return NULL;
	}
	return (gpu_pipeline_t)p;
}

static void webgpu_pipeline_destroy(gpu_pipeline_t pipeline)
{
	struct gpu_pipeline *p = (struct gpu_pipeline *)pipeline;

	if (!p)
		return;
	if (p->pso)
		wgpuRenderPipelineRelease(p->pso);
	g_mem->free(p);
}

static void webgpu_cmd_set_pipeline(gpu_cmd_buf_t cmd, gpu_pipeline_t pipeline)
{
	struct gpu_cmd_buf_state *c = (struct gpu_cmd_buf_state *)cmd;
	struct gpu_pipeline *p = (struct gpu_pipeline *)pipeline;

	if (!c || !c->pass || !p || !p->pso)
		return;
	wgpuRenderPassEncoderSetPipeline(c->pass, p->pso);
}

/* -------------------------------------------------------- render passes */

static void webgpu_cmd_begin_render_pass(gpu_cmd_buf_t cmd,
					 const struct gpu_render_pass_desc *desc)
{
	struct gpu_cmd_buf_state *c = (struct gpu_cmd_buf_state *)cmd;
	WGPURenderPassColorAttachment color[GPU_MAX_COLOR_ATTACHMENTS];
	WGPURenderPassDescriptor rp;
	WGPUSurfaceTexture st;
	uint32_t i;
	uint32_t count;

	if (!c || !c->in_use || c->pass)
		return;

	count = desc->color_count ? desc->color_count : 1;
	if (count > GPU_MAX_COLOR_ATTACHMENTS)
		count = GPU_MAX_COLOR_ATTACHMENTS;

	memset(color, 0, sizeof(color));
	for (i = 0; i < count; i++) {
		const struct gpu_color_attachment *a = &desc->color[i];

		/*
		 * A NULL texture handle means the backbuffer — the convention
		 * fg_import_backbuffer establishes and the WebGL backend already
		 * follows. Offscreen targets arrive with texture_create, which
		 * lands in a later slice, so today every pass is a surface pass.
		 */
		if (desc->color_count == 0 || !a->texture) {
			if (!c->surface_view) {
				memset(&st, 0, sizeof(st));
				wgpuSurfaceGetCurrentTexture(g_surface, &st);
				if (!st.texture)
					return;
				c->surface_tex  = st.texture;
				c->surface_view = wgpuTextureCreateView(st.texture, NULL);
			}
			color[i].view = c->surface_view;
		} else {
			STUB_ONCE("offscreen color attachments");
			return;
		}

		color[i].depthSlice   = WGPU_DEPTH_SLICE_UNDEFINED;
		color[i].loadOp       = (desc->color_count == 0)
						? WGPULoadOp_Clear
						: to_load_op(a->load_op);
		color[i].storeOp      = (desc->color_count == 0)
						? WGPUStoreOp_Store
						: to_store_op(a->store_op);
		color[i].clearValue.r = a->clear[0];
		color[i].clearValue.g = a->clear[1];
		color[i].clearValue.b = a->clear[2];
		color[i].clearValue.a = a->clear[3];
	}

	if (desc->depth)
		STUB_ONCE("depth attachments");

	memset(&rp, 0, sizeof(rp));
	rp.colorAttachmentCount = count;
	rp.colorAttachments     = color;

	c->pass = wgpuCommandEncoderBeginRenderPass(c->enc, &rp);
}

static void webgpu_cmd_end_render_pass(gpu_cmd_buf_t cmd)
{
	struct gpu_cmd_buf_state *c = (struct gpu_cmd_buf_state *)cmd;

	if (!c || !c->pass)
		return;
	wgpuRenderPassEncoderEnd(c->pass);
	wgpuRenderPassEncoderRelease(c->pass);
	c->pass = NULL;
}

/* ------------------------------------------------------------ binding */

static void webgpu_cmd_bind_vertex_buffer(gpu_cmd_buf_t cmd, uint32_t slot,
					  gpu_buffer_t buf, uint32_t offset)
{
	struct gpu_cmd_buf_state *c = (struct gpu_cmd_buf_state *)cmd;
	struct gpu_buffer *b = (struct gpu_buffer *)buf;

	if (!c || !c->pass || !b || !b->buf)
		return;
	wgpuRenderPassEncoderSetVertexBuffer(c->pass, slot, b->buf, offset,
					     b->size - offset);
}

static void webgpu_cmd_bind_index_buffer(gpu_cmd_buf_t cmd, gpu_buffer_t buf,
					 uint32_t offset, gpu_index_format fmt)
{
	struct gpu_cmd_buf_state *c = (struct gpu_cmd_buf_state *)cmd;
	struct gpu_buffer *b = (struct gpu_buffer *)buf;

	if (!c || !c->pass || !b || !b->buf)
		return;
	wgpuRenderPassEncoderSetIndexBuffer(c->pass, b->buf,
					    to_index_format(fmt), offset,
					    b->size - offset);
}

/*
 * Uniform binding needs a bind group cache: WebGPU has no mutable slot state
 * for a draw to inherit, so bindings must be accumulated and resolved into an
 * immutable bind group at draw time. That is the next slice's whole subject.
 */
static void webgpu_cmd_bind_uniform_buffer(gpu_cmd_buf_t cmd, uint32_t slot,
					   gpu_buffer_t buf, uint32_t offset,
					   uint32_t size)
{
	(void)cmd; (void)slot; (void)buf; (void)offset; (void)size;
	STUB_ONCE("cmd_bind_uniform_buffer (needs the bind group cache)");
}

/* --------------------------------------------------------------- draws */

static void webgpu_cmd_draw(gpu_cmd_buf_t cmd, uint32_t vertex_count,
			    uint32_t instance_count, uint32_t first_vertex,
			    uint32_t first_instance)
{
	struct gpu_cmd_buf_state *c = (struct gpu_cmd_buf_state *)cmd;

	if (!c || !c->pass)
		return;
	wgpuRenderPassEncoderDraw(c->pass, vertex_count,
				  instance_count ? instance_count : 1,
				  first_vertex, first_instance);
}

static void webgpu_cmd_draw_indexed(gpu_cmd_buf_t cmd,
				    const struct gpu_draw_indexed_args *args)
{
	struct gpu_cmd_buf_state *c = (struct gpu_cmd_buf_state *)cmd;

	if (!c || !c->pass)
		return;
	wgpuRenderPassEncoderDrawIndexed(c->pass, args->index_count,
					 args->instance_count ? args->instance_count : 1,
					 args->first_index, args->vertex_offset,
					 args->first_instance);
}

/* --------------------------------------------------------------- stubs */

static void webgpu_cmd_barrier(gpu_cmd_buf_t cmd,
			       const struct gpu_barrier *barriers, uint32_t count)
{
	/* WebGPU inserts its own barriers between passes; nothing to do until
	 * compute lands, which is why this is a silent no-op rather than a stub. */
	(void)cmd; (void)barriers; (void)count;
}

static void webgpu_cmd_set_scissor(gpu_cmd_buf_t cmd, int32_t x, int32_t y,
				   uint32_t width, uint32_t height)
{
	(void)cmd; (void)x; (void)y; (void)width; (void)height;
	STUB_ONCE("cmd_set_scissor");
}

static gpu_texture_t webgpu_texture_create(const struct gpu_texture_desc *desc)
{
	(void)desc;
	STUB_ONCE("texture_create");
	return NULL;
}

static void webgpu_texture_destroy(gpu_texture_t texture)
{
	(void)texture;
	STUB_ONCE("texture_destroy");
}

static void webgpu_cmd_bind_texture(gpu_cmd_buf_t cmd, uint32_t unit,
				    gpu_texture_t texture)
{
	(void)cmd; (void)unit; (void)texture;
	STUB_ONCE("cmd_bind_texture (needs the bind group cache)");
}

/*
 * There is no meaningful u32 native handle in WebGPU — the GL texture name this
 * returns is a GL-specific escape hatch for the UI layer. Reworking it together
 * with cmd_bind_texture_native is a breaking change to the vtable contract, so
 * it lands with its consumers in the slice that ports kruddgui.
 */
static uint32_t webgpu_texture_native_handle(gpu_texture_t texture)
{
	(void)texture;
	STUB_ONCE("texture_native_handle (no WebGPU equivalent; see kruddgui slice)");
	return 0;
}

static void webgpu_cmd_bind_texture_native(gpu_cmd_buf_t cmd, uint32_t unit,
					   uint32_t native_handle)
{
	(void)cmd; (void)unit; (void)native_handle;
	STUB_ONCE("cmd_bind_texture_native (no WebGPU equivalent)");
}

static void *webgpu_gpu_malloc(size_t size)
{
	return g_mem->alloc(size);
}

static void webgpu_gpu_free(void *ptr)
{
	g_mem->free(ptr);
}

/* ---------------------------------------------------------- the vtable */

static const struct gpu_api webgpu_api = {
	/* Draws yes; compute is not wired up in this slice. */
	.caps                    = GPU_CAP_DRAW_DIRECT | GPU_CAP_DRAW_INDEXED,
	.cmd_buf_begin           = webgpu_cmd_buf_begin,
	.cmd_buf_submit          = webgpu_cmd_buf_submit,
	.pipeline_create         = webgpu_pipeline_create,
	.pipeline_destroy        = webgpu_pipeline_destroy,
	.cmd_set_pipeline        = webgpu_cmd_set_pipeline,
	.buffer_create           = webgpu_buffer_create,
	.buffer_destroy          = webgpu_buffer_destroy,
	.buffer_update           = webgpu_buffer_update,
	.cmd_bind_vertex_buffer  = webgpu_cmd_bind_vertex_buffer,
	.cmd_bind_index_buffer   = webgpu_cmd_bind_index_buffer,
	.cmd_bind_uniform_buffer = webgpu_cmd_bind_uniform_buffer,
	.cmd_begin_render_pass   = webgpu_cmd_begin_render_pass,
	.cmd_end_render_pass     = webgpu_cmd_end_render_pass,
	.cmd_barrier             = webgpu_cmd_barrier,
	.cmd_draw_indexed        = webgpu_cmd_draw_indexed,
	.cmd_draw                = webgpu_cmd_draw,
	.cmd_set_scissor         = webgpu_cmd_set_scissor,
	.cmd_dispatch            = NULL, /* no compute in this slice */
	.gpu_malloc              = webgpu_gpu_malloc,
	.gpu_free                = webgpu_gpu_free,
	.gpu_host_to_device_ptr  = NULL, /* bindless only */
	.texture_create          = webgpu_texture_create,
	.texture_destroy         = webgpu_texture_destroy,
	.cmd_bind_texture        = webgpu_cmd_bind_texture,
	.texture_native_handle   = webgpu_texture_native_handle,
	.cmd_bind_texture_native = webgpu_cmd_bind_texture_native,
};

/* ------------------------------------------------- surface and device */

/*
 * Match the surface to the canvas and the device's preferred format. Called
 * once the device arrives; a resize hook comes with the full backend.
 */
static void configure_surface(void)
{
	double css_w = 0.0;
	double css_h = 0.0;
	int w;
	int h;
	WGPUSurfaceCapabilities caps;
	WGPUSurfaceConfiguration cfg;

	/*
	 * A WebGPU surface draws into the canvas's backing store, whose size is
	 * the canvas.width/height attributes — not its CSS size. Nothing sets
	 * those in WebGPU mode (the WebGL path used to, via the GL context), so
	 * match the backing store to the element's CSS size here; otherwise the
	 * drawing buffer stays 0x0 and nothing is visible however well the clear
	 * runs.
	 */
	emscripten_get_element_css_size("#canvas", &css_w, &css_h);
	w = (int)css_w;
	h = (int)css_h;
	if (w <= 0)
		w = 800;
	if (h <= 0)
		h = 600;
	emscripten_set_canvas_element_size("#canvas", w, h);

	memset(&caps, 0, sizeof(caps));
	wgpuSurfaceGetCapabilities(g_surface, g_adapter, &caps);
	g_format = (caps.formatCount > 0) ? caps.formats[0]
					  : WGPUTextureFormat_BGRA8Unorm;

	memset(&cfg, 0, sizeof(cfg));
	cfg.device      = g_device;
	cfg.format      = g_format;
	cfg.usage       = WGPUTextureUsage_RenderAttachment;
	cfg.width       = (uint32_t)w;
	cfg.height      = (uint32_t)h;
	cfg.alphaMode   = WGPUCompositeAlphaMode_Opaque;
	cfg.presentMode = WGPUPresentMode_Fifo;
	wgpuSurfaceConfigure(g_surface, &cfg);

	wgpuSurfaceCapabilitiesFreeMembers(caps);

	{
		char buf[96];

		snprintf(buf, sizeof(buf), "webgpu: surface %dx%d format=%d",
			 w, h, (int)g_format);
		webgpu_status(buf);
	}
}

/*
 * Build the triangle through the vtable rather than with bespoke wgpu calls.
 * That is the point of this slice: the same picture as before, but every step
 * — shader lowering, pipeline creation, buffer upload — now goes through the
 * gpu_api entry points the rest of the engine will use.
 */
static void create_triangle(void)
{
	struct gpu_pipeline_desc pd;
	struct gpu_buffer_desc bd;

	memset(&pd, 0, sizeof(pd));
	pd.color_formats[0]     = GPU_FORMAT_BGRA8_UNORM;
	pd.color_format_count   = 0; /* backbuffer: take the surface format */
	pd.topology             = GPU_TOPOLOGY_TRIANGLE_LIST;
	pd.sample_count         = 1;
	pd.depth_format         = GPU_FORMAT_UNKNOWN;

	pd.vertex_layout.attr_count   = 2;
	pd.vertex_layout.stride       = 20;
	pd.vertex_layout.attrs[0].location = 0;
	pd.vertex_layout.attrs[0].offset   = 0;
	pd.vertex_layout.attrs[0].format   = GPU_FORMAT_RG32_FLOAT;
	pd.vertex_layout.attrs[1].location = 1;
	pd.vertex_layout.attrs[1].offset   = 8;
	pd.vertex_layout.attrs[1].format   = GPU_FORMAT_RGB32_FLOAT;

	pd.vert.src     = TRI_SHADER;
	pd.vert.stage   = GPU_SHADER_STAGE_VERTEX;
	pd.vert.dialect = GPU_SHADER_DIALECT_KRUDD;
	pd.frag.src     = TRI_SHADER;
	pd.frag.stage   = GPU_SHADER_STAGE_FRAGMENT;
	pd.frag.dialect = GPU_SHADER_DIALECT_KRUDD;

	g_tri_pso = webgpu_api.pipeline_create(&pd);

	memset(&bd, 0, sizeof(bd));
	bd.size         = sizeof(TRI_VERTS);
	bd.usage        = GPU_BUFFER_USAGE_VERTEX;
	bd.initial_data = TRI_VERTS;
	g_tri_vbuf = webgpu_api.buffer_create(&bd);

	webgpu_status((g_tri_pso && g_tri_vbuf)
		      ? "webgpu: triangle pipeline ready"
		      : "webgpu: triangle pipeline FAILED");
}

static void on_device(WGPURequestDeviceStatus status, WGPUDevice device,
		      WGPUStringView message, void *ud1, void *ud2)
{
	(void)message;
	(void)ud1;
	(void)ud2;
	if (status != WGPURequestDeviceStatus_Success || !device) {
		webgpu_status("webgpu: device request failed");
		g_log->write(LOG_LEVEL_ERROR,
			     "renderer_webgpu: device request failed");
		return;
	}
	g_device = device;
	g_queue  = wgpuDeviceGetQueue(device);
	configure_surface();

	/* The vtable is only usable from here on, so mark ready before building
	 * the triangle — create_triangle goes through the api like any caller. */
	g_ready = 1;
	create_triangle();

	webgpu_announce_renderer();
	webgpu_status("webgpu: device ready — drawing triangle");
	g_log->write(LOG_LEVEL_INFO, "renderer_webgpu: device ready");
}

static void on_adapter(WGPURequestAdapterStatus status, WGPUAdapter adapter,
		       WGPUStringView message, void *ud1, void *ud2)
{
	WGPURequestDeviceCallbackInfo ci;

	(void)message;
	(void)ud1;
	(void)ud2;
	if (status != WGPURequestAdapterStatus_Success || !adapter) {
		webgpu_status("webgpu: no adapter (WebGPU unavailable?)");
		g_log->write(LOG_LEVEL_ERROR,
			     "renderer_webgpu: adapter request failed");
		return;
	}
	g_adapter = adapter;
	webgpu_status("webgpu: adapter ok — requesting device");

	memset(&ci, 0, sizeof(ci));
	ci.mode     = WGPUCallbackMode_AllowSpontaneous;
	ci.callback = on_device;
	wgpuAdapterRequestDevice(adapter, NULL, ci);
}

static void renderer_webgpu_init(void)
{
	WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvas_src;
	WGPUSurfaceDescriptor sd;
	WGPURequestAdapterCallbackInfo ci;

	g_instance = wgpuCreateInstance(NULL);
	if (!g_instance) {
		webgpu_status("webgpu: no instance");
		g_log->write(LOG_LEVEL_ERROR, "renderer_webgpu: no instance");
		return;
	}
	webgpu_status("webgpu: requesting adapter");

	memset(&canvas_src, 0, sizeof(canvas_src));
	canvas_src.chain.sType =
		WGPUSType_EmscriptenSurfaceSourceCanvasHTMLSelector;
	canvas_src.selector = str_view("#canvas");

	memset(&sd, 0, sizeof(sd));
	sd.nextInChain = &canvas_src.chain;
	g_surface = wgpuInstanceCreateSurface(g_instance, &sd);

	memset(&ci, 0, sizeof(ci));
	ci.mode     = WGPUCallbackMode_AllowSpontaneous;
	ci.callback = on_adapter;
	wgpuInstanceRequestAdapter(g_instance, NULL, ci);

	g_log->write(LOG_LEVEL_INFO,
		     "renderer_webgpu: init (requesting adapter)");
}

/*
 * Draw the triangle through the vtable. Once the render cluster runs on this
 * backend the scene renderer owns the frame and this tick goes away; until
 * then it is the only caller, and it deliberately uses nothing the rest of the
 * engine will not.
 */
static void renderer_webgpu_tick(void)
{
	gpu_cmd_buf_t cmd;
	struct gpu_render_pass_desc rp;

	/*
	 * Pump the instance so the adapter/device futures resolve. AllowSpontaneous
	 * callbacks should fire on their own, but processing events each frame is
	 * harmless and covers backends that need the nudge — and it must run before
	 * the not-ready early-out, or the device would never arrive.
	 */
	if (g_instance)
		wgpuInstanceProcessEvents(g_instance);

	if (!g_ready)
		return;

	cmd = webgpu_api.cmd_buf_begin();
	if (!cmd)
		return;

	memset(&rp, 0, sizeof(rp));
	rp.color_count       = 1;
	rp.color[0].texture  = NULL; /* the backbuffer */
	rp.color[0].load_op  = GPU_LOAD_OP_CLEAR;
	rp.color[0].store_op = GPU_STORE_OP_STORE;
	rp.color[0].clear[0] = 0.04f;
	rp.color[0].clear[1] = 0.04f;
	rp.color[0].clear[2] = 0.07f;
	rp.color[0].clear[3] = 1.0f;

	webgpu_api.cmd_begin_render_pass(cmd, &rp);
	if (g_tri_pso && g_tri_vbuf) {
		webgpu_api.cmd_set_pipeline(cmd, g_tri_pso);
		webgpu_api.cmd_bind_vertex_buffer(cmd, 0, g_tri_vbuf, 0);
		webgpu_api.cmd_draw(cmd, 3, 1, 0, 0);
	}
	webgpu_api.cmd_end_render_pass(cmd);
	webgpu_api.cmd_buf_submit(cmd);
}

static void renderer_webgpu_shutdown(void)
{
	if (g_tri_vbuf)
		webgpu_api.buffer_destroy(g_tri_vbuf);
	if (g_tri_pso)
		webgpu_api.pipeline_destroy(g_tri_pso);
	g_tri_vbuf = NULL;
	g_tri_pso  = NULL;
	g_ready    = 0;
	g_log->write(LOG_LEVEL_INFO, "renderer_webgpu: shutdown");
}

static const struct subsystem desc = {
	.name     = "renderer",
	.api      = &webgpu_api,
	.init     = renderer_webgpu_init,
	.tick     = renderer_webgpu_tick,
	.shutdown = renderer_webgpu_shutdown,
};

void renderer_webgpu_plugin_entry(struct subsystem_manager *mgr)
{
	g_log = subsystem_manager_get_api(mgr, "log");
	g_mem = subsystem_manager_get_api(mgr, "memory");
	subsystem_manager_register(mgr, &desc);
}
#else
/*
 * Native builds never drive WebGPU (it is browser-only), so the entry is a
 * no-op that keeps the module compiling and linking into the native image.
 */
void renderer_webgpu_plugin_entry(struct subsystem_manager *mgr)
{
	(void)mgr;
}
#endif
