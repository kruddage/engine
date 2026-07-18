/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * WebGPU renderer backend — the gpu_api vtable.
 *
 * The first slice (#570) stood up the emdawnwebgpu toolchain and drew a single
 * triangle the backend owned outright. This grows that probe into the full
 * struct gpu_api the scene renderer drives through the frame graph: pipelines
 * (with bind groups), vertex/index/uniform buffers, sampled textures, offscreen
 * and backbuffer render passes, and indexed/direct draws — the same vtable the
 * WebGL backend registers, so scene_renderer.c records the same commands whether
 * the page runs ?renderer=webgpu or the default GL path.
 *
 * It is browser-only. Native builds get a no-op plugin entry (bottom of file).
 *
 * Async device init. WebGPU brings up the adapter and device through callbacks,
 * but subsystems boot synchronously and scene_renderer_init() builds its
 * pipelines the moment it registers. So the backend registers itself alone at
 * boot, drives the adapter/device handshake, and exposes renderer_webgpu_ready()
 * — engine.c waits on that and registers the render cluster (frame graph, scene
 * renderer, games, UI) only once the device is live, when the vtable can serve
 * them. The subsystem tick pumps instance events so those futures resolve.
 *
 * Bridging a GL-shaped vtable to WebGPU's rigid format rules. The gpu_api was
 * shaped by the WebGL backend, where a pipeline and a render pass need not agree
 * on attachment formats and an unused binding is harmless. WebGPU demands that a
 * pipeline's color/depth formats exactly match its render pass's attachments and
 * that a bind group match its layout entry-for-entry. Three moves reconcile the
 * two without touching scene_renderer.c:
 *
 *   1. The surface is configured RGBA8Unorm (not the platform-preferred
 *      BGRA8Unorm) so it matches the GPU_FORMAT_RGBA8_UNORM every scene pipeline
 *      declares for its color target; the backbuffer then needs no per-pipeline
 *      format juggling.
 *   2. Render passes begin lazily — at the first cmd_set_pipeline, once the
 *      bound pipeline's exact color/depth shape is known. A pass whose declared
 *      attachments don't cover what the pipeline emits is padded with throwaway
 *      scratch attachments (a depth-only shadow pass gets a scratch color; a
 *      backbuffer 3D pass gets the managed backbuffer depth), so any pipeline is
 *      valid in any pass, exactly as it was on GL.
 *   3. Bind group layouts come from the pipeline's automatic layout, and the set
 *      of bindings each group needs is read straight out of the WGSL the shader
 *      DSL lowers to (its @group/@binding decorations). A draw then builds bind
 *      groups holding precisely those bindings from the buffers/textures the
 *      caller bound, so there is never an extra or missing entry.
 */
#include "renderer.h"
#include "subsystem.h"
#include "subsystem_manager.h"
#include "log_api.h"
#include "memory_api.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5.h>
#include <webgpu/webgpu.h>
#include <stdio.h>
#include "script.h"

/* --- Backend globals ----------------------------------------------------- */

static const struct log_api    *g_log;
static const struct memory_api *g_mem;

static WGPUInstance       g_instance;
static WGPUSurface        g_surface;
static WGPUAdapter        g_adapter;
static WGPUDevice         g_device;
static WGPUQueue          g_queue;
static WGPUTextureFormat  g_format;   /* the surface's color format (RGBA8Unorm) */
static int                g_ready;    /* 1 once device + surface are configured */
static WGPUSampler        g_sampler;  /* one shared linear/repeat sampler */
/*
 * A 1x1 opaque-white texture stood in for any sampler slot that has no valid
 * sampled texture to offer: an unbound slot, or — the reason it exists — a depth
 * texture bound where the shader declares texture_2d<f32>. WGSL cannot sample a
 * Depth32Float through a float texture binding (it needs texture_depth_2d), so
 * the sun-shadow map would make the forward pass's bind group invalid. Swapping
 * in white keeps the bind group legal and the scene renders unshadowed — "fully
 * lit" reads the same as the 1x1 dummy the scene renderer already falls back to.
 * Real shadow sampling lands when the DSL lowers a depth sampler to a depth
 * texture + comparison sampler; this is the seam it plugs into.
 */
static WGPUTextureView    g_white_view;

/* Surface backing-store size, tracked so the backbuffer depth can follow it. */
static uint32_t           g_surf_w;
static uint32_t           g_surf_h;

/*
 * WebGPU has no implicit depth buffer for the surface the way a GL context does
 * (renderer_webgl requested attrs.depth). A 3D pass that targets the backbuffer
 * declares no depth texture of its own (the frame graph only builds depth
 * attachments from declared depth resources, and the forward-to-backbuffer pass
 * declares none), so the backend keeps one depth texture sized to the surface
 * and lends it to any backbuffer pass whose pipeline tests depth. Recreated when
 * the surface resizes.
 */
static WGPUTexture        g_bb_depth;
static WGPUTextureView    g_bb_depth_view;
static uint32_t           g_bb_depth_w;
static uint32_t           g_bb_depth_h;

/*
 * Throwaway attachments that pad a pass to the shape its pipeline emits (see the
 * file banner, move 2): a color target for a depth-only pass whose pipeline
 * still writes color, and a depth buffer for an offscreen color pass whose
 * pipeline still tests depth. Their contents are never read; they exist only to
 * satisfy WebGPU's pipeline/pass format match. Sized up on demand.
 */
static WGPUTexture        g_scratch_color;
static WGPUTextureView    g_scratch_color_view;
static uint32_t           g_scratch_color_w;
static uint32_t           g_scratch_color_h;
static WGPUTexture        g_scratch_depth;
static WGPUTextureView    g_scratch_depth_view;
static uint32_t           g_scratch_depth_w;
static uint32_t           g_scratch_depth_h;

/* --- Handle types (definitions for renderer.h's opaque handles) ---------- */

#define WGPU_MAX_UBO_BINDINGS 8
#define WGPU_MAX_SAMPLERS     8

struct gpu_pipeline {
	WGPURenderPipeline pipe;
	uint32_t           color_count; /* fragment targets the pipeline writes */
	int                has_depth;   /* pipeline tests/writes depth */
	/*
	 * Which bindings each shader group actually declares, read from the
	 * lowered WGSL. Group 0 holds uniform blocks at their block number; group
	 * 1 holds sampler #i as texture binding 2i and sampler binding 2i+1.
	 */
	uint8_t  ubo_bindings[WGPU_MAX_UBO_BINDINGS];
	uint32_t ubo_count;
	uint32_t sampler_count;
};

struct gpu_buffer {
	WGPUBuffer buf;
	uint32_t   size;
};

struct gpu_texture {
	WGPUTexture     tex;
	WGPUTextureView view;
	uint32_t        width;
	uint32_t        height;
	int             is_depth;
};

/*
 * The command buffer is the frame's work in flight. WebGPU is a recording API
 * (unlike WebGL 2's immediate mode), so this holds the live encoder, the open
 * render pass, the bound pipeline, and the binding state a draw assembles into
 * bind groups. One static instance: the engine records one frame at a time.
 */
struct gpu_cmd_buf {
	WGPUCommandEncoder    enc;
	WGPURenderPassEncoder pass;
	int                   pass_open;      /* 1 between the real begin and end */
	struct gpu_pipeline  *pipeline;

	/* Pending pass config captured at cmd_begin_render_pass, applied at the
	 * lazy begin once the pipeline's shape is known. */
	struct gpu_texture   *color_tex;      /* NULL => the backbuffer */
	int                   have_color;     /* the pass declared a color target */
	WGPULoadOp            color_load;
	WGPUColor             clear_color;
	struct gpu_texture   *depth_tex;      /* explicit depth attachment, or NULL */
	WGPULoadOp            depth_load;
	float                 clear_depth;
	uint32_t              pass_w;
	uint32_t              pass_h;

	/* Binding state, sticky across draws; a draw reads what the pipeline needs. */
	struct {
		WGPUBuffer buf;
		uint32_t   offset;
		uint32_t   size;
	} ubo[WGPU_MAX_UBO_BINDINGS];
	struct gpu_texture *tex[WGPU_MAX_SAMPLERS];

	/* The frame's acquired backbuffer, held from first backbuffer pass to
	 * submit; NULL when the frame drew only offscreen (e.g. a thumbnail). */
	WGPUTexture     bb_tex;
	WGPUTextureView bb_view;
};

static struct gpu_cmd_buf g_cmd;

/* --- Shell bridges (defined in shell.html; guarded so a reshuffle is safe) - */

EM_JS(void, webgpu_announce_renderer, (void), {
	if (typeof window.kruddSetRenderer === 'function')
		window.kruddSetRenderer('webgpu');
})

EM_JS(void, webgpu_status, (const char *msg), {
	var s = UTF8ToString(msg);
	if (typeof window.kruddWebGPULog === 'function')
		window.kruddWebGPULog(s);
	if (typeof console !== 'undefined')
		console.log('[webgpu] ' + s);
})

/* --- Small helpers ------------------------------------------------------- */

/* A WGPUStringView over a NUL-terminated C string (the emdawnwebgpu string ABI). */
static WGPUStringView str_view(const char *s)
{
	WGPUStringView v;

	v.data   = s;
	v.length = strlen(s);
	return v;
}

static WGPUTextureFormat translate_format(gpu_format fmt)
{
	switch (fmt) {
	case GPU_FORMAT_RGBA8_UNORM:   return WGPUTextureFormat_RGBA8Unorm;
	case GPU_FORMAT_BGRA8_UNORM:   return WGPUTextureFormat_BGRA8Unorm;
	case GPU_FORMAT_DEPTH32_FLOAT: return WGPUTextureFormat_Depth32Float;
	default:                       return WGPUTextureFormat_RGBA8Unorm;
	}
}

/* Vertex-attribute format -> WGPU. Only the float vectors the layout uses map;
 * anything else is a layout the scene never builds, so it falls back to x2. */
static WGPUVertexFormat translate_vertex_format(gpu_format fmt)
{
	switch (fmt) {
	case GPU_FORMAT_RG32_FLOAT:   return WGPUVertexFormat_Float32x2;
	case GPU_FORMAT_RGB32_FLOAT:  return WGPUVertexFormat_Float32x3;
	case GPU_FORMAT_RGBA32_FLOAT: return WGPUVertexFormat_Float32x4;
	default:                      return WGPUVertexFormat_Float32x2;
	}
}

static WGPUPrimitiveTopology translate_topology(gpu_topology t)
{
	switch (t) {
	case GPU_TOPOLOGY_TRIANGLE_LIST:  return WGPUPrimitiveTopology_TriangleList;
	case GPU_TOPOLOGY_TRIANGLE_STRIP: return WGPUPrimitiveTopology_TriangleStrip;
	case GPU_TOPOLOGY_LINE_LIST:      return WGPUPrimitiveTopology_LineList;
	case GPU_TOPOLOGY_POINT_LIST:     return WGPUPrimitiveTopology_PointList;
	default:                          return WGPUPrimitiveTopology_TriangleList;
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

/*
 * Read the @group/@binding decorations the shader DSL emits into WGSL so a draw
 * can build bind groups that match the pipeline's automatic layout exactly. The
 * lowering pins the scheme (renderer.scm / shader.scm document it): every group
 * 0 binding is a uniform block at its block number; group 1 holds sampler #i as
 * an even binding 2i (the texture) paired with 2i+1 (the sampler). So the count
 * of distinct group-0 bindings gives the uniform blocks, and the highest group-1
 * binding gives the sampler count.
 */
static void scan_bindings(struct gpu_pipeline *p, const char *wgsl)
{
	const char *s = wgsl;
	int         hi_g1 = -1;

	if (!wgsl)
		return;
	while ((s = strstr(s, "@group(")) != NULL) {
		int         group   = atoi(s + 7);
		const char *btok    = strstr(s, "@binding(");

		s += 7;
		if (!btok)
			continue;
		{
			int binding = atoi(btok + 9);

			if (group == 0 && binding < WGPU_MAX_UBO_BINDINGS) {
				uint32_t i;
				int      seen = 0;

				for (i = 0; i < p->ubo_count; i++)
					if (p->ubo_bindings[i] == (uint8_t)binding)
						seen = 1;
				if (!seen && p->ubo_count < WGPU_MAX_UBO_BINDINGS)
					p->ubo_bindings[p->ubo_count++] =
						(uint8_t)binding;
			} else if (group == 1) {
				if (binding > hi_g1)
					hi_g1 = binding;
			}
		}
	}
	/* bindings 0,1 -> one sampler; 0,1,2,3 -> two; i.e. (hi + 1) / 2. */
	if (hi_g1 >= 0) {
		p->sampler_count = (uint32_t)(hi_g1 + 1) / 2u;
		if (p->sampler_count > WGPU_MAX_SAMPLERS)
			p->sampler_count = WGPU_MAX_SAMPLERS;
	}
}

/* Grow a scratch color target to at least w x h, returning its view. */
static WGPUTextureView ensure_scratch_color(uint32_t w, uint32_t h)
{
	WGPUTextureDescriptor td;

	if (g_scratch_color && g_scratch_color_w >= w && g_scratch_color_h >= h)
		return g_scratch_color_view;
	if (w < g_scratch_color_w) w = g_scratch_color_w;
	if (h < g_scratch_color_h) h = g_scratch_color_h;
	if (g_scratch_color_view) wgpuTextureViewRelease(g_scratch_color_view);
	if (g_scratch_color)      wgpuTextureRelease(g_scratch_color);

	memset(&td, 0, sizeof(td));
	td.usage         = WGPUTextureUsage_RenderAttachment;
	td.dimension     = WGPUTextureDimension_2D;
	td.size.width    = w;
	td.size.height   = h;
	td.size.depthOrArrayLayers = 1;
	td.format        = g_format;
	td.mipLevelCount = 1;
	td.sampleCount   = 1;
	g_scratch_color      = wgpuDeviceCreateTexture(g_device, &td);
	g_scratch_color_view = wgpuTextureCreateView(g_scratch_color, NULL);
	g_scratch_color_w    = w;
	g_scratch_color_h    = h;
	return g_scratch_color_view;
}

/* Grow a scratch depth target to at least w x h, returning its view. */
static WGPUTextureView ensure_scratch_depth(uint32_t w, uint32_t h)
{
	WGPUTextureDescriptor td;

	if (g_scratch_depth && g_scratch_depth_w >= w && g_scratch_depth_h >= h)
		return g_scratch_depth_view;
	if (w < g_scratch_depth_w) w = g_scratch_depth_w;
	if (h < g_scratch_depth_h) h = g_scratch_depth_h;
	if (g_scratch_depth_view) wgpuTextureViewRelease(g_scratch_depth_view);
	if (g_scratch_depth)      wgpuTextureRelease(g_scratch_depth);

	memset(&td, 0, sizeof(td));
	td.usage         = WGPUTextureUsage_RenderAttachment;
	td.dimension     = WGPUTextureDimension_2D;
	td.size.width    = w;
	td.size.height   = h;
	td.size.depthOrArrayLayers = 1;
	td.format        = WGPUTextureFormat_Depth32Float;
	td.mipLevelCount = 1;
	td.sampleCount   = 1;
	g_scratch_depth      = wgpuDeviceCreateTexture(g_device, &td);
	g_scratch_depth_view = wgpuTextureCreateView(g_scratch_depth, NULL);
	g_scratch_depth_w    = w;
	g_scratch_depth_h    = h;
	return g_scratch_depth_view;
}

/* The managed backbuffer depth, grown to the surface size. */
static WGPUTextureView ensure_backbuffer_depth(uint32_t w, uint32_t h)
{
	WGPUTextureDescriptor td;

	if (g_bb_depth && g_bb_depth_w == w && g_bb_depth_h == h)
		return g_bb_depth_view;
	if (g_bb_depth_view) wgpuTextureViewRelease(g_bb_depth_view);
	if (g_bb_depth)      wgpuTextureRelease(g_bb_depth);

	memset(&td, 0, sizeof(td));
	td.usage         = WGPUTextureUsage_RenderAttachment;
	td.dimension     = WGPUTextureDimension_2D;
	td.size.width    = w;
	td.size.height   = h;
	td.size.depthOrArrayLayers = 1;
	td.format        = WGPUTextureFormat_Depth32Float;
	td.mipLevelCount = 1;
	td.sampleCount   = 1;
	g_bb_depth      = wgpuDeviceCreateTexture(g_device, &td);
	g_bb_depth_view = wgpuTextureCreateView(g_bb_depth, NULL);
	g_bb_depth_w    = w;
	g_bb_depth_h    = h;
	return g_bb_depth_view;
}

/* --- Surface / device bring-up ------------------------------------------- */

/*
 * Match the surface backing store to the canvas CSS size and configure it. The
 * format is forced to RGBA8Unorm so it agrees with the color format the scene
 * pipelines declare (see the file banner). A resize hook lands with the real
 * viewport plumbing; for now the size is sampled once here.
 */
static void configure_surface(void)
{
	double                   css_w = 0.0, css_h = 0.0;
	int                      w, h;
	WGPUSurfaceConfiguration cfg;

	emscripten_get_element_css_size("#canvas", &css_w, &css_h);
	w = (int)css_w;
	h = (int)css_h;
	if (w <= 0) w = 800;
	if (h <= 0) h = 600;
	emscripten_set_canvas_element_size("#canvas", w, h);

	g_format = WGPUTextureFormat_RGBA8Unorm;
	g_surf_w = (uint32_t)w;
	g_surf_h = (uint32_t)h;

	memset(&cfg, 0, sizeof(cfg));
	cfg.device      = g_device;
	cfg.format      = g_format;
	cfg.usage       = WGPUTextureUsage_RenderAttachment;
	cfg.width       = (uint32_t)w;
	cfg.height      = (uint32_t)h;
	cfg.alphaMode   = WGPUCompositeAlphaMode_Opaque;
	cfg.presentMode = WGPUPresentMode_Fifo;
	wgpuSurfaceConfigure(g_surface, &cfg);

	{
		char buf[96];

		snprintf(buf, sizeof(buf), "webgpu: surface %dx%d format=%d",
			 w, h, (int)g_format);
		webgpu_status(buf);
	}
}

/* Bake the 1x1 opaque-white sampled-texture fallback (see g_white_view). */
static void create_white_texture(void)
{
	static const unsigned char WHITE[4] = { 255, 255, 255, 255 };
	WGPUTextureDescriptor      td;
	WGPUTexelCopyTextureInfo   dst;
	WGPUTexelCopyBufferLayout  layout;
	WGPUExtent3D               ext;
	WGPUTexture                tex;

	memset(&td, 0, sizeof(td));
	td.usage         = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
	td.dimension     = WGPUTextureDimension_2D;
	td.size.width    = 1;
	td.size.height   = 1;
	td.size.depthOrArrayLayers = 1;
	td.format        = WGPUTextureFormat_RGBA8Unorm;
	td.mipLevelCount = 1;
	td.sampleCount   = 1;
	tex = wgpuDeviceCreateTexture(g_device, &td);

	memset(&dst, 0, sizeof(dst));
	dst.texture = tex;
	dst.aspect  = WGPUTextureAspect_All;
	memset(&layout, 0, sizeof(layout));
	layout.bytesPerRow  = 4;
	layout.rowsPerImage = 1;
	memset(&ext, 0, sizeof(ext));
	ext.width = 1; ext.height = 1; ext.depthOrArrayLayers = 1;
	wgpuQueueWriteTexture(g_queue, &dst, WHITE, sizeof(WHITE), &layout, &ext);

	g_white_view = wgpuTextureCreateView(tex, NULL);
}

static void create_default_sampler(void)
{
	WGPUSamplerDescriptor sd;

	memset(&sd, 0, sizeof(sd));
	sd.addressModeU  = WGPUAddressMode_Repeat;
	sd.addressModeV  = WGPUAddressMode_Repeat;
	sd.addressModeW  = WGPUAddressMode_Repeat;
	sd.magFilter     = WGPUFilterMode_Linear;
	sd.minFilter     = WGPUFilterMode_Linear;
	sd.mipmapFilter  = WGPUMipmapFilterMode_Linear;
	sd.lodMinClamp   = 0.0f;
	sd.lodMaxClamp   = 32.0f;
	sd.maxAnisotropy = 1;
	g_sampler = wgpuDeviceCreateSampler(g_device, &sd);
}

static void on_device(WGPURequestDeviceStatus status, WGPUDevice device,
		      WGPUStringView message, void *ud1, void *ud2)
{
	(void)message; (void)ud1; (void)ud2;
	if (status != WGPURequestDeviceStatus_Success || !device) {
		webgpu_status("webgpu: device request failed");
		g_log->write(LOG_LEVEL_ERROR,
			     "renderer_webgpu: device request failed");
		return;
	}
	g_device = device;
	g_queue  = wgpuDeviceGetQueue(device);
	configure_surface();
	create_default_sampler();
	create_white_texture();
	g_ready = 1;
	webgpu_announce_renderer();
	webgpu_status("webgpu: device ready — gpu_api live");
	g_log->write(LOG_LEVEL_INFO, "renderer_webgpu: device ready");
}

static void on_adapter(WGPURequestAdapterStatus status, WGPUAdapter adapter,
		       WGPUStringView message, void *ud1, void *ud2)
{
	WGPURequestDeviceCallbackInfo ci;

	(void)message; (void)ud1; (void)ud2;
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

/* --- gpu_api: command buffer --------------------------------------------- */

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

	/* The browser presents the surface on the next animation frame; releasing
	 * the acquired texture/view here just drops our references. */
	if (c->bb_view) wgpuTextureViewRelease(c->bb_view);
	if (c->bb_tex)  wgpuTextureRelease(c->bb_tex);
	c->bb_view = NULL;
	c->bb_tex  = NULL;
}

/* --- gpu_api: pipelines --------------------------------------------------- */

/* Lower one stage's DSL source to WGSL through the runtime, else pass a raw
 * WGSL/GLSL string as-is. Only the krudd DSL is lowered here (as the WebGL
 * backend lowers to GLSL); the scene speaks the DSL, so that is the live path. */
static WGPUShaderModule stage_module(const struct gpu_shader_source *s,
				     const char *label)
{
	const char *code = s->src;

	if (!code)
		return NULL;
	if (s->dialect == GPU_SHADER_DIALECT_KRUDD) {
		code = script_shader_transpile_wgsl(s->src, label);
		if (!code) {
			g_log->write(LOG_LEVEL_ERROR,
				     "renderer_webgpu: %s WGSL transpile failed",
				     label);
			return NULL;
		}
	}
	return make_module(code);
}

static gpu_pipeline_t
webgpu_pipeline_create(const struct gpu_pipeline_desc *desc)
{
	struct gpu_pipeline         *p;
	const char                  *vs_wgsl, *fs_wgsl;
	WGPUShaderModule             vmod, fmod;
	WGPUVertexAttribute          attrs[GPU_MAX_VERTEX_ATTRS];
	WGPUVertexBufferLayout       vblayout;
	WGPUColorTargetState         target;
	WGPUBlendState               blend;
	WGPUFragmentState            frag;
	WGPUDepthStencilState        depth;
	WGPURenderPipelineDescriptor pd;
	uint32_t                     i;

	p = g_mem->alloc(sizeof(*p));
	if (!p)
		return NULL;
	memset(p, 0, sizeof(*p));
	if (!g_ready)
		return p; /* handle exists but stays inert until the device is up */

	/*
	 * Transpile both stages, then scan the emitted WGSL for the group/binding
	 * layout a draw will fill. Scan the raw strings before creating modules so
	 * a failed transpile bails without leaking a module.
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
	scan_bindings(p, vs_wgsl);
	scan_bindings(p, fs_wgsl);

	vmod = make_module(vs_wgsl);
	fmod = make_module(fs_wgsl);

	memset(attrs, 0, sizeof(attrs));
	for (i = 0; i < desc->vertex_layout.attr_count &&
		    i < GPU_MAX_VERTEX_ATTRS; i++) {
		attrs[i].format = translate_vertex_format(
			desc->vertex_layout.attrs[i].format);
		attrs[i].offset = desc->vertex_layout.attrs[i].offset;
		attrs[i].shaderLocation = desc->vertex_layout.attrs[i].location;
	}
	memset(&vblayout, 0, sizeof(vblayout));
	vblayout.arrayStride    = desc->vertex_layout.stride;
	vblayout.stepMode       = WGPUVertexStepMode_Vertex;
	vblayout.attributeCount = desc->vertex_layout.attr_count;
	vblayout.attributes     = attrs;

	/* Straight-alpha compositing when the pipeline asks for blending (a 2D
	 * overlay), else the opaque default every scene pipeline relies on. */
	memset(&blend, 0, sizeof(blend));
	blend.color.operation = WGPUBlendOperation_Add;
	blend.color.srcFactor = WGPUBlendFactor_SrcAlpha;
	blend.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
	blend.alpha.operation = WGPUBlendOperation_Add;
	blend.alpha.srcFactor = WGPUBlendFactor_One;
	blend.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;

	memset(&target, 0, sizeof(target));
	target.format    = g_format;
	target.writeMask = WGPUColorWriteMask_All;
	if (desc->blend_enable)
		target.blend = &blend;

	memset(&frag, 0, sizeof(frag));
	frag.module      = fmod;
	frag.entryPoint  = str_view("fs_main");
	frag.targetCount = desc->color_format_count ? 1u : 0u;
	frag.targets     = desc->color_format_count ? &target : NULL;

	p->color_count = desc->color_format_count ? 1u : 0u;
	p->has_depth   = (desc->depth_format == GPU_FORMAT_DEPTH32_FLOAT);

	memset(&depth, 0, sizeof(depth));
	depth.format            = WGPUTextureFormat_Depth32Float;
	/* Depth test on by default (as WebGL's begin_render_pass established); a
	 * 2D overlay pipeline disables it, matching disable_depth_test. */
	depth.depthWriteEnabled = desc->disable_depth_test
				  ? WGPUOptionalBool_False : WGPUOptionalBool_True;
	depth.depthCompare      = desc->disable_depth_test
				  ? WGPUCompareFunction_Always : WGPUCompareFunction_Less;

	memset(&pd, 0, sizeof(pd));
	pd.layout             = NULL; /* automatic layout, reflected per draw */
	pd.vertex.module      = vmod;
	pd.vertex.entryPoint  = str_view("vs_main");
	pd.vertex.bufferCount = desc->vertex_layout.attr_count ? 1u : 0u;
	pd.vertex.buffers     = desc->vertex_layout.attr_count ? &vblayout : NULL;
	pd.primitive.topology = translate_topology(desc->topology);
	pd.primitive.frontFace = WGPUFrontFace_CCW;
	pd.primitive.cullMode  = WGPUCullMode_None;
	pd.multisample.count   = 1;
	pd.multisample.mask    = 0xFFFFFFFFu;
	pd.fragment            = desc->color_format_count ? &frag : NULL;
	pd.depthStencil        = p->has_depth ? &depth : NULL;

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
	if (g_cmd.pipeline == p)
		g_cmd.pipeline = NULL;
	if (p->pipe)
		wgpuRenderPipelineRelease(p->pipe);
	g_mem->free(p);
}

/* --- gpu_api: buffers ----------------------------------------------------- */

static WGPUBufferUsage buffer_usage(uint32_t usage)
{
	WGPUBufferUsage u = WGPUBufferUsage_CopyDst;

	if (usage & GPU_BUFFER_USAGE_VERTEX)  u |= WGPUBufferUsage_Vertex;
	if (usage & GPU_BUFFER_USAGE_INDEX)   u |= WGPUBufferUsage_Index;
	if (usage & GPU_BUFFER_USAGE_UNIFORM) u |= WGPUBufferUsage_Uniform;
	if (usage & GPU_BUFFER_USAGE_STORAGE) u |= WGPUBufferUsage_Storage;
	return u;
}

static gpu_buffer_t webgpu_buffer_create(const struct gpu_buffer_desc *desc)
{
	struct gpu_buffer   *b;
	WGPUBufferDescriptor bd;
	/* WebGPU buffer sizes must be a multiple of 4; round up so a tightly
	 * packed vertex/uniform blob is always a legal allocation. */
	uint32_t             size = (uint32_t)((desc->size + 3u) & ~3u);

	b = g_mem->alloc(sizeof(*b));
	if (!b)
		return NULL;
	memset(b, 0, sizeof(*b));
	b->size = size;
	if (!g_ready)
		return b;

	memset(&bd, 0, sizeof(bd));
	bd.usage = buffer_usage(desc->usage);
	bd.size  = size;
	b->buf   = wgpuDeviceCreateBuffer(g_device, &bd);
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
	if (b->buf)
		wgpuBufferRelease(b->buf);
	g_mem->free(b);
}

static void webgpu_buffer_update(gpu_buffer_t buf, uint32_t offset,
				 const void *data, uint32_t size)
{
	struct gpu_buffer *b = (struct gpu_buffer *)buf;
	uint32_t           padded;

	if (!b || !b->buf || !g_ready)
		return;
	/* wgpuQueueWriteBuffer requires a 4-byte-multiple size; a Material block
	 * can be an odd byte count, so round up (the buffer was sized with slack). */
	padded = (size + 3u) & ~3u;
	if (offset + padded > b->size)
		padded = b->size > offset ? b->size - offset : 0u;
	if (padded)
		wgpuQueueWriteBuffer(g_queue, b->buf, offset, data, padded);
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

static void webgpu_cmd_bind_index_buffer(gpu_cmd_buf_t cmd, gpu_buffer_t buf,
					 uint32_t offset, gpu_index_format fmt)
{
	struct gpu_cmd_buf *c = (struct gpu_cmd_buf *)cmd;
	struct gpu_buffer  *b = (struct gpu_buffer *)buf;
	WGPUIndexFormat     f = (fmt == GPU_INDEX_FORMAT_UINT32)
				? WGPUIndexFormat_Uint32 : WGPUIndexFormat_Uint16;

	if (!c->pass_open || !b || !b->buf)
		return;
	wgpuRenderPassEncoderSetIndexBuffer(c->pass, b->buf, f, offset,
					    WGPU_WHOLE_SIZE);
}

static void webgpu_cmd_bind_uniform_buffer(gpu_cmd_buf_t cmd, uint32_t slot,
					   gpu_buffer_t buf, uint32_t offset,
					   uint32_t size)
{
	struct gpu_cmd_buf *c = (struct gpu_cmd_buf *)cmd;
	struct gpu_buffer  *b = (struct gpu_buffer *)buf;

	if (slot >= WGPU_MAX_UBO_BINDINGS)
		return;
	/* Record the binding; the draw folds it into bind group 0 at the block
	 * number `slot`, which is exactly the WGSL @binding the block lowered to. */
	c->ubo[slot].buf    = b ? b->buf : NULL;
	c->ubo[slot].offset = offset;
	c->ubo[slot].size   = (size + 3u) & ~3u; /* uniform bindings round to 4 */
}

/* --- gpu_api: render passes ----------------------------------------------- */

static void
webgpu_cmd_begin_render_pass(gpu_cmd_buf_t cmd,
			     const struct gpu_render_pass_desc *desc)
{
	struct gpu_cmd_buf *c = (struct gpu_cmd_buf *)cmd;
	struct gpu_texture *color0 =
		desc->color_count > 0
			? (struct gpu_texture *)desc->color[0].texture
			: NULL;

	if (!g_ready || !c->enc)
		return;

	/*
	 * Capture the pass configuration; the real WGPU render pass opens lazily
	 * at the first cmd_set_pipeline, when the pipeline's exact attachment
	 * shape is known and scratch padding can be chosen (see the file banner).
	 */
	c->pass_open   = 0;
	c->pipeline    = NULL;
	c->have_color  = desc->color_count > 0;
	c->color_tex   = color0;              /* NULL => backbuffer */
	c->color_load  = (desc->color_count > 0 &&
			  desc->color[0].load_op == GPU_LOAD_OP_CLEAR)
			 ? WGPULoadOp_Clear : WGPULoadOp_Load;
	if (desc->color_count > 0) {
		c->clear_color.r = desc->color[0].clear[0];
		c->clear_color.g = desc->color[0].clear[1];
		c->clear_color.b = desc->color[0].clear[2];
		c->clear_color.a = desc->color[0].clear[3];
	}
	c->depth_tex   = (struct gpu_texture *)desc->depth;
	/* An explicit depth attachment honours the pass's load op; a synthesized
	 * one (backbuffer/scratch) is always cleared — it is freshly attached each
	 * frame with undefined contents, so loading it would read garbage. */
	c->depth_load  = (desc->depth_load_op == GPU_LOAD_OP_CLEAR)
			 ? WGPULoadOp_Clear : WGPULoadOp_Load;
	c->clear_depth = desc->depth_load_op == GPU_LOAD_OP_CLEAR
			 ? desc->clear_depth : 1.0f;

	/* Pass dimensions: an explicit attachment's size, else the surface. */
	if (color0) {
		c->pass_w = color0->width;
		c->pass_h = color0->height;
	} else if (c->depth_tex) {
		c->pass_w = c->depth_tex->width;
		c->pass_h = c->depth_tex->height;
	} else {
		c->pass_w = g_surf_w;
		c->pass_h = g_surf_h;
	}
}

/* Open the real WGPU render pass, padding attachments to the pipeline's shape. */
static void begin_pass_now(struct gpu_cmd_buf *c, struct gpu_pipeline *p)
{
	WGPURenderPassColorAttachment       col;
	WGPURenderPassDepthStencilAttachment depth;
	WGPURenderPassDescriptor            rp;
	WGPUTextureView                     color_view = NULL;
	WGPUTextureView                     depth_view = NULL;
	int                                 depth_explicit = 0;

	/* Color: the declared target if the pass has one, else a scratch target
	 * when the pipeline still writes color (a depth-only shadow pass). */
	if (p->color_count > 0) {
		if (c->have_color) {
			if (c->color_tex) {
				color_view = c->color_tex->view;
			} else if (c->bb_view) {
				/* A later backbuffer pass in the same frame reuses
				 * the texture already acquired for this frame. */
				color_view = c->bb_view;
			} else {
				/* Backbuffer: acquire this frame's surface texture. */
				WGPUSurfaceTexture st;

				memset(&st, 0, sizeof(st));
				wgpuSurfaceGetCurrentTexture(g_surface, &st);
				if (st.texture) {
					c->bb_tex  = st.texture;
					c->bb_view = wgpuTextureCreateView(
						st.texture, NULL);
					color_view = c->bb_view;
				}
			}
		} else {
			color_view = ensure_scratch_color(c->pass_w, c->pass_h);
		}
	}

	/* Depth: the explicit attachment, the managed backbuffer depth for a
	 * backbuffer 3D pass, or a scratch depth for an offscreen 3D pass. */
	if (p->has_depth) {
		if (c->depth_tex) {
			depth_view     = c->depth_tex->view;
			depth_explicit = 1;
		} else if (!c->color_tex && c->have_color) {
			depth_view = ensure_backbuffer_depth(c->pass_w, c->pass_h);
		} else {
			depth_view = ensure_scratch_depth(c->pass_w, c->pass_h);
		}
	}

	memset(&col, 0, sizeof(col));
	col.view       = color_view;
	col.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
	col.loadOp     = c->have_color ? c->color_load : WGPULoadOp_Clear;
	col.storeOp    = WGPUStoreOp_Store;
	col.clearValue = c->clear_color;

	memset(&depth, 0, sizeof(depth));
	depth.view            = depth_view;
	/* Synthesized depth is always cleared; an explicit attachment honours the
	 * pass's load op (which is Clear for every depth pass this renderer runs). */
	depth.depthLoadOp     = depth_explicit ? c->depth_load : WGPULoadOp_Clear;
	depth.depthStoreOp    = WGPUStoreOp_Store;
	depth.depthClearValue = c->clear_depth;

	memset(&rp, 0, sizeof(rp));
	rp.colorAttachmentCount     = color_view ? 1u : 0u;
	rp.colorAttachments         = color_view ? &col : NULL;
	rp.depthStencilAttachment   = depth_view ? &depth : NULL;

	c->pass      = wgpuCommandEncoderBeginRenderPass(c->enc, &rp);
	c->pass_open = 1;
}

static void webgpu_cmd_set_pipeline(gpu_cmd_buf_t cmd, gpu_pipeline_t pipeline)
{
	struct gpu_cmd_buf  *c = (struct gpu_cmd_buf *)cmd;
	struct gpu_pipeline *p = (struct gpu_pipeline *)pipeline;

	if (!g_ready || !c->enc || !p || !p->pipe)
		return;
	c->pipeline = p;
	if (!c->pass_open)
		begin_pass_now(c, p); /* first pipeline in the pass fixes its shape */
	wgpuRenderPassEncoderSetPipeline(c->pass, p->pipe);
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
	c->pipeline  = NULL;
}

static void webgpu_cmd_barrier(gpu_cmd_buf_t cmd,
			       const struct gpu_barrier *barriers, uint32_t count)
{
	(void)cmd; (void)barriers; (void)count;
	/* WebGPU inserts pass-to-pass synchronisation itself; barriers are no-ops. */
}

/* --- gpu_api: draws ------------------------------------------------------- */

/*
 * Build and set the bind groups the bound pipeline needs, from the sticky
 * binding state, right before a draw. Group 0 carries the uniform blocks the
 * shader declared (at their block numbers); group 1 carries sampler #i as the
 * texture (binding 2i) and the shared sampler (2i+1). The layouts come from the
 * pipeline's automatic layout, so the entries here match it exactly. Bind groups
 * and the queried layouts are released straight after SetBindGroup — the command
 * encoder retains them for the duration of the pass (Dawn's C ownership rule).
 */
static void apply_bind_groups(struct gpu_cmd_buf *c)
{
	struct gpu_pipeline *p = c->pipeline;

	if (p->ubo_count > 0) {
		WGPUBindGroupLayout   bgl;
		WGPUBindGroupEntry    e[WGPU_MAX_UBO_BINDINGS];
		WGPUBindGroupDescriptor bd;
		WGPUBindGroup         bg;
		uint32_t              i;

		memset(e, 0, sizeof(e));
		for (i = 0; i < p->ubo_count; i++) {
			uint32_t b = p->ubo_bindings[i];

			e[i].binding = b;
			e[i].buffer  = c->ubo[b].buf;
			e[i].offset  = c->ubo[b].offset;
			e[i].size    = c->ubo[b].size ? c->ubo[b].size
						      : WGPU_WHOLE_SIZE;
		}
		bgl = wgpuRenderPipelineGetBindGroupLayout(p->pipe, 0);
		memset(&bd, 0, sizeof(bd));
		bd.layout     = bgl;
		bd.entryCount = p->ubo_count;
		bd.entries    = e;
		bg = wgpuDeviceCreateBindGroup(g_device, &bd);
		wgpuRenderPassEncoderSetBindGroup(c->pass, 0, bg, 0, NULL);
		wgpuBindGroupRelease(bg);
		wgpuBindGroupLayoutRelease(bgl);
	}

	if (p->sampler_count > 0) {
		WGPUBindGroupLayout     bgl;
		WGPUBindGroupEntry      e[WGPU_MAX_SAMPLERS * 2];
		WGPUBindGroupDescriptor bd;
		WGPUBindGroup           bg;
		uint32_t                i, n = 0;

		memset(e, 0, sizeof(e));
		for (i = 0; i < p->sampler_count; i++) {
			struct gpu_texture *t = c->tex[i];

			e[n].binding     = 2u * i;
			/* A missing or depth texture can't back a float sampler
			 * binding; the white fallback keeps the group valid. */
			e[n].textureView = (t && !t->is_depth && t->view)
					   ? t->view : g_white_view;
			n++;
			e[n].binding = 2u * i + 1u;
			e[n].sampler = g_sampler;
			n++;
		}
		bgl = wgpuRenderPipelineGetBindGroupLayout(p->pipe, 1);
		memset(&bd, 0, sizeof(bd));
		bd.layout     = bgl;
		bd.entryCount = n;
		bd.entries    = e;
		bg = wgpuDeviceCreateBindGroup(g_device, &bd);
		wgpuRenderPassEncoderSetBindGroup(c->pass, 1, bg, 0, NULL);
		wgpuBindGroupRelease(bg);
		wgpuBindGroupLayoutRelease(bgl);
	}
}

static void webgpu_cmd_draw_indexed(gpu_cmd_buf_t cmd,
				    const struct gpu_draw_indexed_args *args)
{
	struct gpu_cmd_buf *c = (struct gpu_cmd_buf *)cmd;

	if (!c->pass_open || !c->pipeline)
		return;
	apply_bind_groups(c);
	wgpuRenderPassEncoderDrawIndexed(c->pass, args->index_count,
					 args->instance_count, args->first_index,
					 args->vertex_offset, args->first_instance);
}

static void webgpu_cmd_draw(gpu_cmd_buf_t cmd, uint32_t vertex_count,
			    uint32_t instance_count, uint32_t first_vertex,
			    uint32_t first_instance)
{
	struct gpu_cmd_buf *c = (struct gpu_cmd_buf *)cmd;

	if (!c->pass_open || !c->pipeline)
		return;
	apply_bind_groups(c);
	wgpuRenderPassEncoderDraw(c->pass, vertex_count, instance_count,
				  first_vertex, first_instance);
}

static void webgpu_cmd_set_scissor(gpu_cmd_buf_t cmd, int32_t x, int32_t y,
				   uint32_t width, uint32_t height)
{
	struct gpu_cmd_buf *c = (struct gpu_cmd_buf *)cmd;

	if (!c->pass_open)
		return;
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	wgpuRenderPassEncoderSetScissorRect(c->pass, (uint32_t)x, (uint32_t)y,
					    width, height);
}

/* --- gpu_api: allocation / textures --------------------------------------- */

static void *webgpu_gpu_malloc(size_t size) { return g_mem->alloc(size); }
static void  webgpu_gpu_free(void *ptr)     { g_mem->free(ptr); }

static gpu_texture_t webgpu_texture_create(const struct gpu_texture_desc *desc)
{
	struct gpu_texture   *t;
	WGPUTextureDescriptor td;
	int                   is_depth =
		(desc->format == GPU_FORMAT_DEPTH32_FLOAT);

	t = g_mem->alloc(sizeof(*t));
	if (!t)
		return NULL;
	memset(t, 0, sizeof(*t));
	t->width    = desc->width;
	t->height   = desc->height;
	t->is_depth = is_depth;
	if (!g_ready)
		return t;

	memset(&td, 0, sizeof(td));
	/* Every texture can be a render attachment; sampled ones (and any with
	 * initial data to copy in) also get TextureBinding / CopyDst. */
	td.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding;
	if (desc->initial_data)
		td.usage |= WGPUTextureUsage_CopyDst;
	td.dimension     = WGPUTextureDimension_2D;
	td.size.width    = desc->width;
	td.size.height   = desc->height;
	td.size.depthOrArrayLayers = 1;
	td.format        = translate_format(desc->format);
	td.mipLevelCount = 1; /* generate_mips is a follow-up (a blit chain) */
	td.sampleCount   = desc->sample_count ? desc->sample_count : 1;
	t->tex  = wgpuDeviceCreateTexture(g_device, &td);
	t->view = t->tex ? wgpuTextureCreateView(t->tex, NULL) : NULL;

	if (t->tex && desc->initial_data && !is_depth) {
		/* Upload tightly-packed RGBA8 level 0 (a baked procedural texture);
		 * the wire format matches texture_blob, as on the GL backend. */
		WGPUTexelCopyTextureInfo   dst;
		WGPUTexelCopyBufferLayout  layout;
		WGPUExtent3D               ext;

		memset(&dst, 0, sizeof(dst));
		dst.texture = t->tex;
		dst.aspect  = WGPUTextureAspect_All;
		memset(&layout, 0, sizeof(layout));
		layout.bytesPerRow  = desc->width * 4u;
		layout.rowsPerImage = desc->height;
		memset(&ext, 0, sizeof(ext));
		ext.width              = desc->width;
		ext.height             = desc->height;
		ext.depthOrArrayLayers = 1;
		wgpuQueueWriteTexture(g_queue, &dst, desc->initial_data,
				      (size_t)desc->width * desc->height * 4u,
				      &layout, &ext);
	}
	return t;
}

static void webgpu_texture_destroy(gpu_texture_t texture)
{
	struct gpu_texture *t = (struct gpu_texture *)texture;

	if (!t)
		return;
	if (t->view) wgpuTextureViewRelease(t->view);
	if (t->tex)  wgpuTextureRelease(t->tex);
	g_mem->free(t);
}

static void webgpu_cmd_bind_texture(gpu_cmd_buf_t cmd, uint32_t unit,
				    gpu_texture_t texture)
{
	struct gpu_cmd_buf *c = (struct gpu_cmd_buf *)cmd;

	if (unit >= WGPU_MAX_SAMPLERS)
		return;
	/* Record the texture; the draw places it at sampler slot `unit` (WGSL
	 * group 1, binding 2*unit), which mirrors the GL backend's texture unit. */
	c->tex[unit] = (struct gpu_texture *)texture;
}

/*
 * A native GL-name escape hatch the UI layer uses to composite a texture it
 * holds by raw handle. WebGPU has no such integer name (textures are opaque
 * objects), so there is nothing to hand back and nothing to bind by. Returning
 * 0 tells kruddgui's kgui-image "no native handle", which it already tolerates
 * (the GL backend's contract); wiring the UI's external-texture path through
 * WebGPU is the follow-up that lands with the UI backend.
 */
static uint32_t webgpu_texture_native_handle(gpu_texture_t texture)
{
	(void)texture;
	return 0u;
}

static void webgpu_cmd_bind_texture_native(gpu_cmd_buf_t cmd, uint32_t unit,
					   uint32_t native_handle)
{
	(void)cmd; (void)unit; (void)native_handle;
}

/* --- vtable + subsystem --------------------------------------------------- */

static const struct gpu_api webgpu_api = {
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
	.cmd_dispatch            = NULL, /* compute is a later capability */
	.gpu_malloc              = webgpu_gpu_malloc,
	.gpu_free                = webgpu_gpu_free,
	.gpu_host_to_device_ptr  = NULL, /* bindless-only; not a WebGPU cap */
	.texture_create          = webgpu_texture_create,
	.texture_destroy         = webgpu_texture_destroy,
	.cmd_bind_texture        = webgpu_cmd_bind_texture,
	.texture_native_handle   = webgpu_texture_native_handle,
	.cmd_bind_texture_native = webgpu_cmd_bind_texture_native,
};

static void renderer_webgpu_init(void)
{
	WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvas_src;
	WGPUSurfaceDescriptor         sd;
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
 * The scene renderer drives per-frame drawing through the vtable, so this tick
 * only pumps instance events — that is what lets the async adapter/device
 * futures resolve during bring-up (and is harmless once ready).
 */
static void renderer_webgpu_tick(void)
{
	if (g_instance)
		wgpuInstanceProcessEvents(g_instance);
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

/* Has the device finished its async handshake? engine.c waits on this before
 * registering the render cluster, whose init builds pipelines on the vtable. */
int renderer_webgpu_ready(void)
{
	return g_ready;
}

void renderer_webgpu_plugin_entry(struct subsystem_manager *mgr)
{
	g_log = subsystem_manager_get_api(mgr, "log");
	g_mem = subsystem_manager_get_api(mgr, "memory");
	subsystem_manager_register(mgr, &desc);
}
#else
/*
 * Native builds never drive WebGPU (it is browser-only), so the entry is a
 * no-op that keeps the module compiling and linking into the native image; the
 * readiness query answers "never ready" for any native caller.
 */
void renderer_webgpu_plugin_entry(struct subsystem_manager *mgr)
{
	(void)mgr;
}

int renderer_webgpu_ready(void)
{
	return 0;
}
#endif
