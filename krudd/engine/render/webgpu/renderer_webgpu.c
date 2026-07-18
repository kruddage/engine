/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * WebGPU renderer backend — rung 1 of the incremental epic (#572).
 *
 * The #570 probe drew a triangle with hand-owned wgpu* calls inlined in the
 * tick. This rung draws the same triangle, but entirely through a registered
 * struct gpu_api vtable — the shape every backend exposes and the scene
 * renderer eventually drives. Nothing above the backend changes yet: the frame
 * graph, scene renderer, textures, uniforms, and depth are later rungs, so the
 * tick still owns the frame and drives the vtable itself.
 *
 * The surface is configured RGBA8Unorm so it matches the color format pipelines
 * declare (GPU_FORMAT_RGBA8_UNORM); the backbuffer then needs no per-pipeline
 * format juggling. A render pass whose color attachment carries a NULL texture
 * handle targets the backbuffer — the same convention the WebGL backend uses
 * (see fg_import_backbuffer), so scene_renderer.c will record identical commands
 * on either backend when it lands.
 *
 * It is browser-only. Selection lives in engine.c: with ?renderer=webgpu the
 * engine registers this backend alone and skips the GL render cluster. The
 * device handshake is async (adapter -> device callbacks); the tick no-ops
 * until the device is ready, then draws the triangle every frame.
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

/*
 * Internal definitions for the opaque handle types declared in renderer.h.
 * A caller never dereferences these; the backend owns their layout.
 */
struct gpu_pipeline {
	WGPURenderPipeline pipe;
};

struct gpu_buffer {
	WGPUBuffer buf;
	uint32_t   size;
};

/*
 * The command buffer is the frame's work in flight. WebGPU is a recording API
 * (unlike WebGL 2's immediate mode), so this holds the live encoder and the
 * open render pass. One static instance: the engine records one frame at a
 * time. bb_tex/bb_view are this frame's acquired backbuffer, held from
 * begin_render_pass to submit so the drop happens after the queue submit.
 */
struct gpu_cmd_buf {
	WGPUCommandEncoder    enc;
	WGPURenderPassEncoder pass;
	int                   pass_open;
	WGPUTexture           bb_tex;
	WGPUTextureView       bb_view;
};

static const struct log_api    *g_log;
static const struct memory_api *g_mem;

static WGPUInstance      g_instance;
static WGPUSurface       g_surface;
static WGPUAdapter       g_adapter;
static WGPUDevice        g_device;
static WGPUQueue         g_queue;
static WGPUTextureFormat g_format;   /* the surface's color format (RGBA8Unorm) */
static uint32_t          g_surf_w;
static uint32_t          g_surf_h;
static int               g_ready;    /* 1 once the device + surface are live */

static struct gpu_cmd_buf g_cmd;

static gpu_pipeline_t g_tri_pipeline; /* the triangle's pipeline */
static gpu_buffer_t   g_tri_vbuf;     /* interleaved pos.xy + col.rgb, 3 verts */

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
 * Surface the probe's progress into the shell's scrolling WebGPU log panel (and
 * the console), so a browser without a working adapter/device reports where it
 * stopped, with full history, instead of just a blank canvas. The epic's
 * verification note calls console/DOM evidence reliable where pixel capture is
 * not, so this is the signal a build box can trust.
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

static WGPUPrimitiveTopology translate_topology(gpu_topology topo)
{
	switch (topo) {
	case GPU_TOPOLOGY_TRIANGLE_LIST:  return WGPUPrimitiveTopology_TriangleList;
	case GPU_TOPOLOGY_TRIANGLE_STRIP: return WGPUPrimitiveTopology_TriangleStrip;
	case GPU_TOPOLOGY_LINE_LIST:      return WGPUPrimitiveTopology_LineList;
	case GPU_TOPOLOGY_POINT_LIST:     return WGPUPrimitiveTopology_PointList;
	default:                          return WGPUPrimitiveTopology_TriangleList;
	}
}

/* Map a float-vector vertex-attribute format to its WGSL vertex format. */
static WGPUVertexFormat translate_vertex_format(gpu_format fmt)
{
	switch (fmt) {
	case GPU_FORMAT_RG32_FLOAT:   return WGPUVertexFormat_Float32x2;
	case GPU_FORMAT_RGB32_FLOAT:  return WGPUVertexFormat_Float32x3;
	case GPU_FORMAT_RGBA32_FLOAT: return WGPUVertexFormat_Float32x4;
	default:                      return WGPUVertexFormat_Float32x3;
	}
}

/*
 * Match the surface backing store to the canvas CSS size and configure it. The
 * format is forced to RGBA8Unorm so it agrees with the color format the scene
 * pipelines declare. A resize hook lands with later viewport plumbing; for now
 * the size is sampled once here.
 */
static void configure_surface(void)
{
	double                   css_w = 0.0;
	double                   css_h = 0.0;
	int                      w;
	int                      h;
	WGPUSurfaceConfiguration cfg;

	/*
	 * A WebGPU surface draws into the canvas's backing store, whose size is
	 * the canvas.width/height attributes — not its CSS size. Nothing sets
	 * those in WebGPU mode, so match the backing store to the element's CSS
	 * size here; otherwise the drawing buffer stays 0x0 and nothing is visible
	 * however well the pass runs.
	 */
	emscripten_get_element_css_size("#canvas", &css_w, &css_h);
	w = (int)css_w;
	h = (int)css_h;
	if (w <= 0)
		w = 800;
	if (h <= 0)
		h = 600;
	emscripten_set_canvas_element_size("#canvas", w, h);

	g_format = WGPUTextureFormat_RGBA8Unorm;
	g_surf_w = (uint32_t)w;
	g_surf_h = (uint32_t)h;

	memset(&cfg, 0, sizeof(cfg));
	cfg.device      = g_device;
	cfg.format      = g_format;
	cfg.usage       = WGPUTextureUsage_RenderAttachment;
	cfg.width       = g_surf_w;
	cfg.height      = g_surf_h;
	cfg.alphaMode   = WGPUCompositeAlphaMode_Opaque;
	cfg.presentMode = WGPUPresentMode_Fifo;
	wgpuSurfaceConfigure(g_surface, &cfg);

	{
		char buf[96];

		snprintf(buf, sizeof(buf), "webgpu: surface %ux%u format=%d",
			 g_surf_w, g_surf_h, (int)g_format);
		webgpu_status(buf);
	}
}

/* Compile one WGSL stage string into a shader module. */
static WGPUShaderModule make_module(const char *wgsl)
{
	WGPUShaderSourceWGSL       src;
	WGPUShaderModuleDescriptor desc;

	memset(&src, 0, sizeof(src));
	src.chain.sType = WGPUSType_ShaderSourceWGSL;
	src.code        = str_view(wgsl);
	memset(&desc, 0, sizeof(desc));
	desc.nextInChain = &src.chain;
	return wgpuDeviceCreateShaderModule(g_device, &desc);
}

/* --- gpu_api vtable ------------------------------------------------------- */

static gpu_cmd_buf_t webgpu_cmd_buf_begin(void)
{
	if (!g_ready)
		return &g_cmd;
	memset(&g_cmd, 0, sizeof(g_cmd));
	g_cmd.enc = wgpuDeviceCreateCommandEncoder(g_device, NULL);
	return &g_cmd;
}

static void webgpu_cmd_buf_submit(gpu_cmd_buf_t cmd)
{
	struct gpu_cmd_buf *c = (struct gpu_cmd_buf *)cmd;
	WGPUCommandBuffer   cb;

	if (!g_ready || !c->enc)
		return;
	cb = wgpuCommandEncoderFinish(c->enc, NULL);
	wgpuQueueSubmit(g_queue, 1, &cb);
	wgpuCommandBufferRelease(cb);
	wgpuCommandEncoderRelease(c->enc);
	c->enc = NULL;

	/*
	 * The browser presents the surface on the next animation frame; releasing
	 * the acquired texture/view here just drops our references to this frame's
	 * backbuffer.
	 */
	if (c->bb_view)
		wgpuTextureViewRelease(c->bb_view);
	if (c->bb_tex)
		wgpuTextureRelease(c->bb_tex);
	c->bb_view = NULL;
	c->bb_tex  = NULL;
}

static gpu_pipeline_t webgpu_pipeline_create(const struct gpu_pipeline_desc *desc)
{
	struct gpu_pipeline         *p;
	const char                  *vs_wgsl;
	const char                  *fs_wgsl;
	WGPUShaderModule             vmod;
	WGPUShaderModule             fmod;
	WGPUVertexAttribute          attrs[GPU_MAX_VERTEX_ATTRS];
	WGPUVertexBufferLayout       vblayout;
	WGPUColorTargetState         target;
	WGPUFragmentState            frag;
	WGPURenderPipelineDescriptor pd;
	uint32_t                     i;

	p = g_mem->alloc(sizeof(*p));
	if (!p)
		return NULL;
	memset(p, 0, sizeof(*p));
	if (!g_ready)
		return p; /* handle exists but stays inert until the device is up */

	/*
	 * Lower each stage from the krudd DSL to WGSL through the runtime; a raw
	 * WGSL source (dialect != krudd) is handed through unchanged.
	 */
	vs_wgsl = (desc->vert.dialect == GPU_SHADER_DIALECT_KRUDD)
		  ? script_shader_transpile_wgsl(desc->vert.src, "vertex")
		  : desc->vert.src;
	fs_wgsl = (desc->frag.dialect == GPU_SHADER_DIALECT_KRUDD)
		  ? script_shader_transpile_wgsl(desc->frag.src, "fragment")
		  : desc->frag.src;
	if (!vs_wgsl || !fs_wgsl) {
		g_log->write(LOG_LEVEL_ERROR,
			     "renderer_webgpu: pipeline shader transpile failed");
		g_mem->free(p);
		return NULL;
	}
	vmod = make_module(vs_wgsl);
	fmod = make_module(fs_wgsl);

	memset(attrs, 0, sizeof(attrs));
	for (i = 0; i < desc->vertex_layout.attr_count &&
		    i < GPU_MAX_VERTEX_ATTRS; i++) {
		attrs[i].format = translate_vertex_format(
			desc->vertex_layout.attrs[i].format);
		attrs[i].offset         = desc->vertex_layout.attrs[i].offset;
		attrs[i].shaderLocation = desc->vertex_layout.attrs[i].location;
	}
	memset(&vblayout, 0, sizeof(vblayout));
	vblayout.arrayStride    = desc->vertex_layout.stride;
	vblayout.stepMode       = WGPUVertexStepMode_Vertex;
	vblayout.attributeCount = desc->vertex_layout.attr_count;
	vblayout.attributes     = attrs;

	memset(&target, 0, sizeof(target));
	target.format    = g_format;
	target.writeMask = WGPUColorWriteMask_All;

	memset(&frag, 0, sizeof(frag));
	frag.module      = fmod;
	frag.entryPoint  = str_view("fs_main");
	frag.targetCount = 1;
	frag.targets     = &target;

	memset(&pd, 0, sizeof(pd));
	pd.layout              = NULL; /* automatic layout — no bind groups yet */
	pd.vertex.module       = vmod;
	pd.vertex.entryPoint   = str_view("vs_main");
	pd.vertex.bufferCount  = 1;
	pd.vertex.buffers      = &vblayout;
	pd.primitive.topology  = translate_topology(desc->topology);
	pd.primitive.frontFace = WGPUFrontFace_CCW;
	pd.primitive.cullMode  = WGPUCullMode_None;
	pd.multisample.count   = 1;
	pd.multisample.mask    = 0xFFFFFFFFu;
	pd.fragment            = &frag;
	p->pipe = wgpuDeviceCreateRenderPipeline(g_device, &pd);

	wgpuShaderModuleRelease(vmod);
	wgpuShaderModuleRelease(fmod);

	if (!p->pipe) {
		g_log->write(LOG_LEVEL_ERROR,
			     "renderer_webgpu: pipeline create failed");
		g_mem->free(p);
		return NULL;
	}
	return p;
}

static void webgpu_pipeline_destroy(gpu_pipeline_t pipeline)
{
	struct gpu_pipeline *p = (struct gpu_pipeline *)pipeline;

	if (!p)
		return;
	if (p->pipe)
		wgpuRenderPipelineRelease(p->pipe);
	g_mem->free(p);
}

static void webgpu_cmd_set_pipeline(gpu_cmd_buf_t cmd, gpu_pipeline_t pipeline)
{
	struct gpu_cmd_buf  *c = (struct gpu_cmd_buf *)cmd;
	struct gpu_pipeline *p = (struct gpu_pipeline *)pipeline;

	if (!c->pass_open || !p || !p->pipe)
		return;
	wgpuRenderPassEncoderSetPipeline(c->pass, p->pipe);
}

static gpu_buffer_t webgpu_buffer_create(const struct gpu_buffer_desc *desc)
{
	struct gpu_buffer   *b;
	WGPUBufferDescriptor bd;
	/*
	 * WebGPU buffer sizes must be a multiple of 4; round up so a tightly
	 * packed vertex blob is always a legal allocation.
	 */
	uint32_t             size = (uint32_t)((desc->size + 3u) & ~3u);

	b = g_mem->alloc(sizeof(*b));
	if (!b)
		return NULL;
	memset(b, 0, sizeof(*b));
	b->size = size;
	if (!g_ready || size == 0)
		return b; /* a 0-size buffer can't be bound; leave buf NULL (safe) */

	memset(&bd, 0, sizeof(bd));
	bd.usage = WGPUBufferUsage_CopyDst;
	if (desc->usage & GPU_BUFFER_USAGE_VERTEX)
		bd.usage |= WGPUBufferUsage_Vertex;
	if (desc->usage & GPU_BUFFER_USAGE_INDEX)
		bd.usage |= WGPUBufferUsage_Index;
	if (desc->usage & GPU_BUFFER_USAGE_UNIFORM)
		bd.usage |= WGPUBufferUsage_Uniform;
	if (desc->usage & GPU_BUFFER_USAGE_STORAGE)
		bd.usage |= WGPUBufferUsage_Storage;
	bd.size = size;
	b->buf  = wgpuDeviceCreateBuffer(g_device, &bd);
	if (b->buf && desc->initial_data)
		wgpuQueueWriteBuffer(g_queue, b->buf, 0, desc->initial_data,
				     desc->size);
	return b;
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

static void webgpu_cmd_bind_vertex_buffer(gpu_cmd_buf_t cmd, uint32_t slot,
					  gpu_buffer_t buf, uint32_t offset)
{
	struct gpu_cmd_buf *c = (struct gpu_cmd_buf *)cmd;
	struct gpu_buffer  *b = (struct gpu_buffer *)buf;

	if (!c->pass_open || !b || !b->buf)
		return;
	wgpuRenderPassEncoderSetVertexBuffer(c->pass, slot, b->buf, offset,
					     WGPU_WHOLE_SIZE);
}

static void
webgpu_cmd_begin_render_pass(gpu_cmd_buf_t cmd,
			     const struct gpu_render_pass_desc *desc)
{
	struct gpu_cmd_buf                *c = (struct gpu_cmd_buf *)cmd;
	WGPURenderPassColorAttachment      col;
	WGPURenderPassDescriptor           rp;
	WGPUSurfaceTexture                 st;

	if (!g_ready || !c->enc)
		return;

	/*
	 * A NULL color texture handle means the backbuffer (the fg_import_backbuffer
	 * convention). This rung only ever draws to the backbuffer; a real texture
	 * attachment (offscreen target) arrives with a later rung.
	 */
	if (desc->color_count == 0 || desc->color[0].texture != NULL)
		return;

	memset(&st, 0, sizeof(st));
	wgpuSurfaceGetCurrentTexture(g_surface, &st);
	if (!st.texture)
		return;
	c->bb_tex  = st.texture;
	c->bb_view = wgpuTextureCreateView(st.texture, NULL);

	memset(&col, 0, sizeof(col));
	col.view       = c->bb_view;
	col.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
	col.loadOp     = desc->color[0].load_op == GPU_LOAD_OP_CLEAR
			 ? WGPULoadOp_Clear : WGPULoadOp_Load;
	col.storeOp    = WGPUStoreOp_Store;
	col.clearValue.r = desc->color[0].clear[0];
	col.clearValue.g = desc->color[0].clear[1];
	col.clearValue.b = desc->color[0].clear[2];
	col.clearValue.a = desc->color[0].clear[3];

	memset(&rp, 0, sizeof(rp));
	rp.colorAttachmentCount = 1;
	rp.colorAttachments     = &col;

	c->pass      = wgpuCommandEncoderBeginRenderPass(c->enc, &rp);
	c->pass_open = 1;
}

static void webgpu_cmd_end_render_pass(gpu_cmd_buf_t cmd)
{
	struct gpu_cmd_buf *c = (struct gpu_cmd_buf *)cmd;

	if (!c->pass_open)
		return;
	wgpuRenderPassEncoderEnd(c->pass);
	wgpuRenderPassEncoderRelease(c->pass);
	c->pass      = NULL;
	c->pass_open = 0;
}

static void webgpu_cmd_draw(gpu_cmd_buf_t cmd, uint32_t vertex_count,
			    uint32_t instance_count, uint32_t first_vertex,
			    uint32_t first_instance)
{
	struct gpu_cmd_buf *c = (struct gpu_cmd_buf *)cmd;

	if (!c->pass_open)
		return;
	wgpuRenderPassEncoderDraw(c->pass, vertex_count, instance_count,
				  first_vertex, first_instance);
}

/*
 * WebGPU: draw yes; compute is a later capability. Only the rung-1 subset is
 * wired; every other slot stays NULL until the rung that needs it lands.
 */
static const struct gpu_api webgpu_api = {
	.caps                   = GPU_CAP_DRAW_DIRECT | GPU_CAP_DRAW_INDEXED,
	.cmd_buf_begin          = webgpu_cmd_buf_begin,
	.cmd_buf_submit         = webgpu_cmd_buf_submit,
	.pipeline_create        = webgpu_pipeline_create,
	.pipeline_destroy       = webgpu_pipeline_destroy,
	.cmd_set_pipeline       = webgpu_cmd_set_pipeline,
	.buffer_create          = webgpu_buffer_create,
	.buffer_destroy         = webgpu_buffer_destroy,
	.cmd_bind_vertex_buffer = webgpu_cmd_bind_vertex_buffer,
	.cmd_begin_render_pass  = webgpu_cmd_begin_render_pass,
	.cmd_end_render_pass    = webgpu_cmd_end_render_pass,
	.cmd_draw               = webgpu_cmd_draw,
};

/*
 * Build the triangle's pipeline and vertex buffer through the vtable, proving
 * pipeline_create and buffer_create end to end. Called once the device exists.
 */
static void create_triangle(void)
{
	struct gpu_pipeline_desc pd;
	struct gpu_buffer_desc   bd;

	memset(&pd, 0, sizeof(pd));
	pd.color_formats[0]     = GPU_FORMAT_RGBA8_UNORM;
	pd.color_format_count   = 1;
	pd.topology             = GPU_TOPOLOGY_TRIANGLE_LIST;
	pd.vertex_layout.attrs[0].location = 0;
	pd.vertex_layout.attrs[0].offset   = 0;
	pd.vertex_layout.attrs[0].format   = GPU_FORMAT_RG32_FLOAT;  /* a_pos */
	pd.vertex_layout.attrs[1].location = 1;
	pd.vertex_layout.attrs[1].offset   = 8;
	pd.vertex_layout.attrs[1].format   = GPU_FORMAT_RGB32_FLOAT; /* a_col */
	pd.vertex_layout.attr_count = 2;
	pd.vertex_layout.stride     = 20;
	pd.vert.src     = TRI_SHADER;
	pd.vert.stage   = GPU_SHADER_STAGE_VERTEX;
	pd.vert.dialect = GPU_SHADER_DIALECT_KRUDD;
	pd.frag.src     = TRI_SHADER;
	pd.frag.stage   = GPU_SHADER_STAGE_FRAGMENT;
	pd.frag.dialect = GPU_SHADER_DIALECT_KRUDD;
	g_tri_pipeline = webgpu_api.pipeline_create(&pd);

	memset(&bd, 0, sizeof(bd));
	bd.size         = sizeof(TRI_VERTS);
	bd.usage        = GPU_BUFFER_USAGE_VERTEX;
	bd.initial_data = TRI_VERTS;
	g_tri_vbuf = webgpu_api.buffer_create(&bd);

	webgpu_status((g_tri_pipeline && g_tri_vbuf)
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
	create_triangle();
	g_ready = 1;
	webgpu_announce_renderer();
	webgpu_status("webgpu: device ready — drawing triangle via vtable");
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
	WGPUSurfaceDescriptor          sd;
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
 * Draw the triangle through the vtable every frame. The frame graph and scene
 * renderer are later rungs, so this rung's tick is the sole caller of the
 * vtable — a stand-in for scene_renderer.c, recording the same command shape it
 * will. Pump instance events first so the async adapter/device futures resolve
 * during bring-up (and harmlessly once ready); the not-ready early-out sits
 * after the pump, or the device would never arrive.
 */
static void renderer_webgpu_tick(void)
{
	gpu_cmd_buf_t                cmd;
	struct gpu_render_pass_desc  rp;

	if (g_instance)
		wgpuInstanceProcessEvents(g_instance);

	if (!g_ready || !g_tri_pipeline || !g_tri_vbuf)
		return;

	memset(&rp, 0, sizeof(rp));
	rp.color_count      = 1;
	rp.color[0].texture = NULL; /* backbuffer */
	rp.color[0].load_op = GPU_LOAD_OP_CLEAR;
	rp.color[0].store_op = GPU_STORE_OP_STORE;
	rp.color[0].clear[0] = 0.04f;
	rp.color[0].clear[1] = 0.04f;
	rp.color[0].clear[2] = 0.07f;
	rp.color[0].clear[3] = 1.0f;

	cmd = webgpu_api.cmd_buf_begin();
	webgpu_api.cmd_begin_render_pass(cmd, &rp);
	webgpu_api.cmd_set_pipeline(cmd, g_tri_pipeline);
	webgpu_api.cmd_bind_vertex_buffer(cmd, 0, g_tri_vbuf, 0);
	webgpu_api.cmd_draw(cmd, 3, 1, 0, 0);
	webgpu_api.cmd_end_render_pass(cmd);
	webgpu_api.cmd_buf_submit(cmd);
}

static void renderer_webgpu_shutdown(void)
{
	g_ready = 0;
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
