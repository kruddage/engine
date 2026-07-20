/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * WebGPU renderer backend — the gpu_api vtable.
 *
 * A full gpu_api provider registered as the "renderer" subsystem, the same seam
 * the WebGL backend fills. It lowers the krudd shader DSL to WGSL through the
 * runtime and drives Dawn on both targets (see webgpu_platform.h).
 *
 * What is implemented: command buffers, buffers, pipelines, vertex/index
 * binding, uniform buffers (through a bind-group cache), textures and samplers,
 * render-target and depth textures, the full mip chain, scissor, and both draw
 * entry points. What remains a no-op: resource barriers and compute dispatch —
 * cmd_dispatch and gpu_host_to_device_ptr are deliberately NULL in the vtable,
 * gated by the absence of GPU_CAP_BINDLESS, so no caller reaches them.
 *
 * Selection lives in engine.c: on the web WebGPU is the default, so the engine
 * registers this backend alone and skips the GL render cluster unless the page
 * opts out with ?renderer=webgl (or is running on Firefox, which opts out
 * unconditionally for now — see kruddWantsWebGPU in shell.html.in). The device
 * handshake is async (adapter -> device callbacks); the tick pumps the instance
 * and the rest of the boot waits on renderer_webgpu_device_ready().
 *
 * This file builds on both the web and native targets, against the same Dawn
 * revision either way. Everything that genuinely differs between them lives
 * behind webgpu_platform.h — surface creation, backbuffer size, and status
 * reporting — so that the native build is a debugger on this code rather than a
 * parallel copy of it. See spec-dawn-native-build.
 */
#include "subsystem.h"
#include "subsystem_manager.h"

#include "renderer.h"
#include "log_api.h"
#include "memory_api.h"
#include "script.h"
#include "webgpu_platform.h"
#include "texture_registry.h"

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
static WGPUSampler       g_sampler;  /* one linear/repeat sampler for every unit */
static WGPUSampler       g_depth_sampler; /* non-filtering, for depth textures */
static uint32_t          g_texture_generation; /* bumped on every destroy */

/* The mip-chain blit pipeline (see generate_mip_chain), built lazily on the
 * first texture that asks for mips rather than at device bring-up, since most
 * runs never need it (a shadow map or render target never sets generate_mips). */
static WGPURenderPipeline g_mip_pso;
static WGPUBuffer         g_mip_vbo;

/* Textures the caller has released but this frame's commands may still name.
 * Sized well past the frame graph's transient count so a frame never overruns
 * it in practice; see webgpu_texture_destroy for what happens if one does. */
#define WGPU_MAX_PENDING_DESTROY 64
static struct gpu_texture *g_pending_destroy[WGPU_MAX_PENDING_DESTROY];
static uint32_t            g_pending_destroy_count;

static void flush_pending_destroys(void);

/*
 * The backbuffer's companion depth target. begin_render_pass attaches it to any
 * color pass that declared no depth of its own (the default-framebuffer-has-
 * depth emulation the WebGL path gets for free), and it is resized to match the
 * surface. Not tied to any one pass — it lives for the whole device.
 */
static gpu_texture_t  g_depth;
static uint32_t       g_depth_w, g_depth_h;
/*
 * Last known backbuffer size, cached at the points that already establish it.
 *
 * webgpu_platform_backbuffer_size is a plain getter now — it reads the canvas
 * backing store and no longer writes it — so calling it is no longer destructive
 * on its own. The cache stays because the scissor path wants the size the
 * current pass was actually built for, not whatever the canvas has become
 * mid-frame: kruddgui_tick can resize the backing store between passes, and a
 * scissor computed against the new size would not match the pass in flight.
 */
static uint32_t       g_surface_w, g_surface_h;

/*
 * Rebuild the fallback depth target when the backbuffer changes size. Declared
 * here because the pass path calls it the moment it acquires a surface texture,
 * well above the definition.
 */
static void ensure_depth_target(uint32_t w, uint32_t h);

/*
 * Progress reporting and the renderer badge are host-specific — the shell's DOM
 * log panel on the web, stderr natively. Both live behind webgpu_platform.h;
 * these shorthands keep the call sites below reading the way they did.
 */
#define webgpu_status            webgpu_platform_status
#define webgpu_announce_renderer webgpu_platform_announce_renderer

/* A WGPUStringView over a NUL-terminated C string (the emdawnwebgpu string ABI). */
static WGPUStringView str_view(const char *s)
{
	WGPUStringView v;

	v.data   = s;
	v.length = strlen(s);
	return v;
}

/*
 * Warn once, keyed by call site — a one-shot diagnostic for an unsupported
 * request (a texture format the mip blit can't handle, say) so the console
 * names it once instead of a line every frame.
 */
#define WARN_ONCE(msg)                                                          \
	do {                                                                   \
		static int reported;                                           \
		if (!reported) {                                               \
			reported = 1;                                          \
			g_log->write(LOG_LEVEL_WARN,                           \
				     "renderer_webgpu: " msg);                 \
		}                                                              \
	} while (0)

/* ------------------------------------------------------------ handles */

struct gpu_buffer {
	WGPUBuffer buf;
	uint32_t   size;
};

/*
 * Uniform slots live in bind group 0, one binding per slot, mirroring the
 * scheme shader.scm documents and its tests pin:
 *   uniform block (block N) -> @group(0) @binding(N) var<uniform> u_<Name>
 */
#define WGPU_MAX_UNIFORM_SLOTS 8

/*
 * Bind groups are immutable, so one is needed per distinct set of bound
 * resources. Since #602 every draw takes its own uniform-ring slot, so each
 * draw in a frame binds a DISTINCT {buffer, offset} and needs its own bind
 * group — the old assumption that "a scene rebinds the same few buffers, so the
 * cache hits after the first frame" no longer holds. A frame now needs as many
 * cached bind groups as it has draws through a pipeline; an undersized cache
 * evicts (and releases) one every draw past its size, every frame, for any
 * scene with more draws-per-pipeline than it holds. Eight covered the tiny boot
 * and tic-tac-toe scenes but not a real one: chess stages ~100 pieces + board
 * tiles on the single pbr pipeline. Sized to comfortably cover a full scene so
 * the common case never evicts (and never releases a bind group a recorded draw
 * still references). The proper long-term answer is one bind group with a
 * dynamic offset per draw; this is the interim. See #624.
 */
#define WGPU_BIND_GROUP_CACHE 256

struct uniform_binding {
	WGPUBuffer buf;
	uint32_t   offset;
	uint32_t   size;
};

#define WGPU_MAX_TEXTURE_UNITS 4

/*
 * A cached group holds views, and the frame graph destroys its transient
 * textures every frame -- so an entry can outlive the texture it names, and a
 * freshly allocated view can land on the same address as a dead one, which
 * makes the memcmp below report a hit on a destroyed texture. GENERATION is the
 * guard: any texture_destroy bumps the global counter, and an entry stamped
 * with an older one is treated as a miss.
 */
struct tex_group_entry {
	WGPUBindGroup   bg;
	WGPUTextureView views[WGPU_MAX_TEXTURE_UNITS];
	uint32_t        generation;
	int             valid;
};

struct bind_group_entry {
	WGPUBindGroup          bg;
	struct uniform_binding slots[WGPU_MAX_UNIFORM_SLOTS];
	uint32_t               mask;
	int                    valid;
};

struct gpu_texture {
	WGPUTexture     tex;
	WGPUTextureView view;
	uint32_t        width;
	uint32_t        height;
	uint32_t        mip_levels;
	gpu_format      format;
};

struct gpu_pipeline {
	WGPURenderPipeline pso;
	/*
	 * Which uniform slots this pipeline actually requires. Derived from the
	 * generated WGSL rather than the DSL source, because the transpiler only
	 * emits a block that the stage *uses* — a shader that declares Camera but
	 * never reads it needs no binding, and building a bind group for it would
	 * be a validation error.
	 */
	uint32_t uniform_mask;
	/*
	 * Bindings the shader declares in group 1: a sampled texture at 2i and
	 * its companion sampler at 2i+1, the pairing shader.scm documents.
	 */
	uint32_t texture_mask;
	struct bind_group_entry cache[WGPU_BIND_GROUP_CACHE];
	uint32_t next_evict;
	struct tex_group_entry tex_cache[WGPU_BIND_GROUP_CACHE];
	uint32_t tex_next_evict;
};

/*
 * One command buffer in flight, matching how the engine actually draws: a
 * caller does cmd_buf_begin -> passes -> cmd_buf_submit before the next
 * begin. The WebGL backend makes the same assumption implicitly (it ignores
 * its cmd handle entirely and drives global GL state); this makes it explicit.
 *
 * Note what is NOT here: the backbuffer. A frame contains several of these —
 * the frame graph, kruddgui's overlay, an open preview panel — so anything the
 * frame owns has to outlive any one of them. See g_surface_tex.
 */
struct gpu_cmd_buf_state {
	WGPUCommandEncoder    enc;
	WGPURenderPassEncoder pass;
	int             in_use;

	/*
	 * The current pass's attachment size. cmd_set_scissor needs it twice
	 * over: to mirror the caller's GL-convention y into WebGPU's, and to
	 * clamp the box, which WebGPU validates against the attachment where GL
	 * merely clipped. Set at begin_render_pass, since only the pass knows
	 * what it is drawing into.
	 */
	uint32_t pass_w, pass_h;

	/*
	 * Pending uniform bindings. GL bound these straight into slot state the
	 * next draw inherited; WebGPU has no such state, so they accumulate here
	 * and resolve into an immutable bind group at draw time.
	 */
	struct gpu_pipeline   *pipeline;
	struct uniform_binding uniforms[WGPU_MAX_UNIFORM_SLOTS];
	uint32_t               uniform_mask;
	int                    bindings_dirty;
	WGPUTextureView        textures[WGPU_MAX_TEXTURE_UNITS];
	int                    texture_depth[WGPU_MAX_TEXTURE_UNITS];
	int                    textures_dirty;
};

static struct gpu_cmd_buf_state g_cmd;

/*
 * The frame's backbuffer, acquired lazily by the first pass that names it and
 * released in frame_end — not at submit.
 *
 * This is per-frame state, not per-command-buffer, and the difference is the
 * whole bug it fixes. A frame submits several command buffers; releasing the
 * surface at submit meant the next one acquired a second, blank backbuffer, so
 * whatever drew first was left in a texture the canvas never presented. The
 * scene rendered, then vanished behind kruddgui's overlay drawing onto a fresh
 * one. Acquiring twice in a frame is the failure mode begin_render_pass already
 * warns about; holding it here is what makes that warning enforceable.
 *
 * surface_tex stays NULL on the offscreen (native) path, where the platform
 * owns a persistent target and hands out a fresh view — only the view is
 * released there.
 */
static WGPUTexture     g_surface_tex;
static WGPUTextureView g_surface_view;

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

	/*
	 * The backbuffer is deliberately NOT released here — it belongs to the
	 * frame, and more command buffers are likely still to come in this one.
	 * frame_end owns it.
	 */

	/* The commands naming them are now the queue's problem, not ours. */
	flush_pending_destroys();

	memset(c, 0, sizeof(*c));
}

/*
 * The frame boundary: every subsystem has drawn and submitted, so the canvas
 * texture acquired for this frame can go back. The next frame's first pass
 * acquires a fresh one.
 *
 * Idempotent and safe on a frame that never drew — a tick that acquired nothing
 * releases nothing.
 */
static void webgpu_frame_end(void)
{
	if (g_surface_view) {
		wgpuTextureViewRelease(g_surface_view);
		g_surface_view = NULL;
	}
#ifndef __EMSCRIPTEN__
	/*
	 * Natively a real surface is presented by nobody on our behalf: the
	 * windowed platform (Qt) owns the swapchain, so a drawn frame has to be
	 * pushed to it explicitly, while its backbuffer texture is still the
	 * surface's current one — hence before the release below.
	 *
	 * Guarded three ways so the two paths that must not change don't. The web
	 * build never compiles it (the browser presents through the canvas/rAF,
	 * and emdawnwebgpu ships no wgpuSurfacePresent). The offscreen native path
	 * has no surface (g_surface NULL) and skips it. And a tick that acquired
	 * no backbuffer this frame (g_surface_tex NULL) presented nothing, so it
	 * has nothing to push.
	 */
	if (g_surface_tex && g_surface)
		wgpuSurfacePresent(g_surface);
#endif
	if (g_surface_tex) {
		wgpuTextureRelease(g_surface_tex);
		g_surface_tex = NULL;
	}
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

/*
 * wgpuQueueWriteBuffer requires the length be a multiple of 4; GL's
 * glBufferData did not, so callers sized for the looser API land here with odd
 * lengths (a 6-index triangle list at 2 bytes an index, for one). Pad through a
 * staging copy rather than rounding the length up over the caller's buffer,
 * which would read past the end of whatever they handed us.
 */
static void queue_write(WGPUBuffer buf, uint32_t offset, const void *data,
			uint32_t size)
{
	uint32_t aligned = (size + 3u) & ~3u;
	void *tmp;

	if (!buf || !data || !size)
		return;

	if (offset & 3u) {
		g_log->write(LOG_LEVEL_ERROR,
			     "renderer_webgpu: buffer write offset %u is not 4-byte aligned",
			     offset);
		return;
	}

	if (aligned == size) {
		wgpuQueueWriteBuffer(g_queue, buf, offset, data, size);
		return;
	}

	tmp = g_mem->alloc(aligned);
	if (!tmp)
		return;
	memcpy(tmp, data, size);
	memset((unsigned char *)tmp + size, 0, aligned - size);
	wgpuQueueWriteBuffer(g_queue, buf, offset, tmp, aligned);
	g_mem->free(tmp);
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
		queue_write(b->buf, 0, desc->initial_data, (uint32_t)desc->size);
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

	if (!b || !b->buf)
		return;
	queue_write(b->buf, offset, data, size);
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

/*
 * Which uniform slots a lowered stage requires, as a bitmask over binding
 * indices. Scans for the exact declaration shader.scm emits and its tests pin:
 *
 *   @group(0) @binding(N) var<uniform> u_Name : Name;
 *
 * Reading the generated WGSL rather than the DSL is deliberate: the transpiler
 * emits a block only where the stage uses it, so this reflects what the
 * pipeline genuinely requires. Bind group 1 (textures) is skipped here — that
 * arrives with texture support.
 */
static uint32_t scan_group_slots(const char *wgsl, int group)
{
	char needle[24];
	size_t nlen;
	uint32_t mask = 0;
	const char *p = wgsl;

	if (!wgsl)
		return 0;
	snprintf(needle, sizeof(needle), "@group(%d) @binding(", group);
	nlen = strlen(needle);

	while ((p = strstr(p, needle)) != NULL) {
		unsigned slot = 0;
		const char *d = p + nlen;

		if (*d < '0' || *d > '9') {
			p = d;
			continue;
		}
		while (*d >= '0' && *d <= '9')
			slot = slot * 10u + (unsigned)(*d++ - '0');
		if (slot < 32u)
			mask |= 1u << slot;
		else
			g_log->write(LOG_LEVEL_ERROR,
				     "renderer_webgpu: binding %u in group %d is out of range",
				     slot, group);
		p = d;
	}
	return mask;
}

/*
 * Find or build the bind group matching the currently pending uniform bindings
 * for this pipeline. The cache key is the whole binding set, since a bind group
 * is only reusable when every buffer, offset and size matches.
 */
static WGPUBindGroup resolve_bind_group(struct gpu_pipeline *p,
					const struct uniform_binding *want)
{
	WGPUBindGroupEntry entries[WGPU_MAX_UNIFORM_SLOTS];
	WGPUBindGroupDescriptor bd;
	WGPUBindGroupLayout layout;
	struct bind_group_entry *slot;
	uint32_t i;
	uint32_t n = 0;

	for (i = 0; i < WGPU_BIND_GROUP_CACHE; i++) {
		struct bind_group_entry *e = &p->cache[i];

		if (!e->valid || e->mask != p->uniform_mask)
			continue;
		if (memcmp(e->slots, want, sizeof(e->slots)) == 0)
			return e->bg;
	}

	memset(entries, 0, sizeof(entries));
	for (i = 0; i < WGPU_MAX_UNIFORM_SLOTS; i++) {
		if (!(p->uniform_mask & (1u << i)))
			continue;
		if (!want[i].buf) {
			g_log->write(LOG_LEVEL_ERROR,
				     "renderer_webgpu: pipeline needs uniform slot %u but nothing is bound",
				     i);
			return NULL;
		}
		entries[n].binding = i;
		entries[n].buffer  = want[i].buf;
		entries[n].offset  = want[i].offset;
		entries[n].size    = want[i].size;
		n++;
	}

	/*
	 * Pipelines are created with an auto layout, so the layout Dawn derived
	 * from the shader is the authority on what group 0 looks like — no need
	 * to reconstruct it here and risk disagreeing with the shader.
	 */
	layout = wgpuRenderPipelineGetBindGroupLayout(p->pso, 0);
	if (!layout)
		return NULL;

	memset(&bd, 0, sizeof(bd));
	bd.layout     = layout;
	bd.entryCount = n;
	bd.entries    = entries;

	slot = &p->cache[p->next_evict];
	if (slot->valid && slot->bg)
		wgpuBindGroupRelease(slot->bg);
	p->next_evict = (p->next_evict + 1u) % WGPU_BIND_GROUP_CACHE;

	slot->bg = wgpuDeviceCreateBindGroup(g_device, &bd);
	memcpy(slot->slots, want, sizeof(slot->slots));
	slot->mask  = p->uniform_mask;
	slot->valid = slot->bg != NULL;

	wgpuBindGroupLayoutRelease(layout);
	return slot->bg;
}

/*
 * The group-1 companion to resolve_bind_group: a sampled texture at binding 2i
 * and its sampler at 2i+1, which is the pairing shader.scm emits. Colour
 * textures all share the one linear sampler — the engine has never varied
 * filtering per bind — but a depth texture must take the non-filtering one, so
 * the choice is per unit even though there are only ever two samplers.
 */
static WGPUBindGroup resolve_texture_group(struct gpu_pipeline *p,
					   WGPUTextureView *want,
					   const int *want_depth)
{
	WGPUBindGroupEntry entries[WGPU_MAX_TEXTURE_UNITS * 2];
	WGPUBindGroupDescriptor bd;
	WGPUBindGroupLayout layout;
	struct tex_group_entry *slot;
	uint32_t i;
	uint32_t n = 0;

	for (i = 0; i < WGPU_BIND_GROUP_CACHE; i++) {
		struct tex_group_entry *e = &p->tex_cache[i];

		if (e->valid && e->generation == g_texture_generation &&
		    memcmp(e->views, want, sizeof(e->views)) == 0)
			return e->bg;
	}

	memset(entries, 0, sizeof(entries));
	for (i = 0; i < WGPU_MAX_TEXTURE_UNITS; i++) {
		if (!(p->texture_mask & (1u << (i * 2))))
			continue;
		if (!want[i]) {
			g_log->write(LOG_LEVEL_ERROR,
				     "renderer_webgpu: pipeline samples unit %u but no texture is bound",
				     i);
			return NULL;
		}
		entries[n].binding     = i * 2;
		entries[n].textureView = want[i];
		n++;
		entries[n].binding = i * 2 + 1;
		entries[n].sampler = want_depth[i] ? g_depth_sampler : g_sampler;
		n++;
	}

	layout = wgpuRenderPipelineGetBindGroupLayout(p->pso, 1);
	if (!layout)
		return NULL;

	memset(&bd, 0, sizeof(bd));
	bd.layout     = layout;
	bd.entryCount = n;
	bd.entries    = entries;

	slot = &p->tex_cache[p->tex_next_evict];
	if (slot->valid && slot->bg)
		wgpuBindGroupRelease(slot->bg);
	p->tex_next_evict = (p->tex_next_evict + 1u) % WGPU_BIND_GROUP_CACHE;

	slot->bg = wgpuDeviceCreateBindGroup(g_device, &bd);
	memcpy(slot->views, want, sizeof(slot->views));
	slot->generation = g_texture_generation;
	slot->valid = slot->bg != NULL;

	wgpuBindGroupLayoutRelease(layout);
	return slot->bg;
}

/*
 * Called before every draw. A pipeline that requires no uniforms skips the
 * whole path, so a bind-group-less pipeline works unchanged.
 */
static int apply_bindings(struct gpu_cmd_buf_state *c)
{
	WGPUBindGroup bg;

	if (!c->pipeline || !c->pipeline->uniform_mask)
		return 1;
	if (!c->bindings_dirty)
		return 1;

	bg = resolve_bind_group(c->pipeline, c->uniforms);
	if (!bg)
		return 0;

	wgpuRenderPassEncoderSetBindGroup(c->pass, 0, bg, 0, NULL);
	c->bindings_dirty = 0;
	return 1;
}

static int apply_textures(struct gpu_cmd_buf_state *c)
{
	WGPUBindGroup bg;

	if (!c->pipeline || !c->pipeline->texture_mask)
		return 1;
	if (!c->textures_dirty)
		return 1;

	bg = resolve_texture_group(c->pipeline, c->textures, c->texture_depth);
	if (!bg)
		return 0;

	wgpuRenderPassEncoderSetBindGroup(c->pass, 1, bg, 0, NULL);
	c->textures_dirty = 0;
	return 1;
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
	memset(p, 0, sizeof(*p));
	p->uniform_mask = scan_group_slots(vs_wgsl, 0) | scan_group_slots(fs_wgsl, 0);
	p->texture_mask = scan_group_slots(vs_wgsl, 1) | scan_group_slots(fs_wgsl, 1);

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
	memset(&frag, 0, sizeof(frag));
	frag.module      = fmod;
	frag.entryPoint  = str_view("fs_main");
	frag.targetCount = desc->color_format_count;
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
	/*
	 * A depth-only pipeline (the shadow pass) has no fragment state at all.
	 * Keeping the stage with zero targets would leave its @location(0) output
	 * unmatched, which WebGPU rejects; the shader's colour write is simply
	 * unused, exactly as it was when GL discarded it.
	 */
	pd.fragment          = desc->color_format_count ? &frag : NULL;

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
	uint32_t i;

	if (!p)
		return;
	for (i = 0; i < WGPU_BIND_GROUP_CACHE; i++) {
		if (p->cache[i].valid && p->cache[i].bg)
			wgpuBindGroupRelease(p->cache[i].bg);
		if (p->tex_cache[i].valid && p->tex_cache[i].bg)
			wgpuBindGroupRelease(p->tex_cache[i].bg);
	}
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

	/* A different pipeline may want a different bind group for the same
	 * bindings, so the resolved group no longer necessarily applies. */
	if (c->pipeline != p) {
		c->bindings_dirty = 1;
		c->textures_dirty = 1;
	}
	c->pipeline = p;
}

/* -------------------------------------------------------- render passes */


static void webgpu_cmd_begin_render_pass(gpu_cmd_buf_t cmd,
					 const struct gpu_render_pass_desc *desc)
{
	struct gpu_cmd_buf_state *c = (struct gpu_cmd_buf_state *)cmd;
	WGPURenderPassColorAttachment color[GPU_MAX_COLOR_ATTACHMENTS];
	WGPURenderPassDepthStencilAttachment depth_att;
	WGPURenderPassDescriptor rp;
	WGPUSurfaceTexture st;
	uint32_t i;
	uint32_t count;
	int depth_only;
	int targets_backbuffer = 0;

	if (!c || !c->in_use || c->pass)
		return;

	/*
	 * Whether this pass wants the backbuffer has to be settled before any
	 * attachment is built, because acquiring the surface is not free of
	 * consequence: the surface texture is the frame's, and handing it back
	 * to acquire again later in the same frame leaves the draws that follow
	 * in a texture the canvas never shows. A depth-only pass (the sun shadow
	 * map) must therefore never take it in the first place — the colour
	 * fallback below is for a colour pass that named no attachment, which is
	 * the backbuffer by convention, and depth-only is not that case.
	 */
	depth_only = desc->color_count == 0 && desc->depth;

	count = depth_only ? 0 : (desc->color_count ? desc->color_count : 1);
	if (count > GPU_MAX_COLOR_ATTACHMENTS)
		count = GPU_MAX_COLOR_ATTACHMENTS;

	memset(color, 0, sizeof(color));
	for (i = 0; i < count; i++) {
		const struct gpu_color_attachment *a = &desc->color[i];

		/*
		 * A NULL texture handle means the backbuffer — the convention
		 * fg_import_backbuffer establishes and the WebGL backend already
		 * follows. A non-NULL handle is an offscreen target from
		 * texture_create and takes the else branch below (its own view).
		 */
		if (desc->color_count == 0 || !a->texture) {
			targets_backbuffer = 1;
			if (!g_surface_view) {
				if (g_surface) {
					memset(&st, 0, sizeof(st));
					wgpuSurfaceGetCurrentTexture(g_surface, &st);
					if (!st.texture)
						return;
					g_surface_tex  = st.texture;
					/*
					 * The acquired texture is the authority on
					 * the backbuffer's size — it is what every
					 * other attachment in this pass must match.
					 * Ask it rather than recomputing from the
					 * canvas: kruddgui_tick can resize the
					 * backing store between frames, and at
					 * devicePixelRatio != 1 a depth target built
					 * from CSS pixels fails validation against
					 * this texture's device pixels.
					 */
					g_surface_w = wgpuTextureGetWidth(st.texture);
					g_surface_h = wgpuTextureGetHeight(st.texture);
					ensure_depth_target(g_surface_w, g_surface_h);
					g_surface_view =
						wgpuTextureCreateView(st.texture, NULL);
				} else {
					/*
					 * Offscreen: the platform owns a
					 * persistent colour target and hands
					 * out a fresh view, so surface_tex
					 * stays NULL and only the view is
					 * released at end of frame.
					 */
					g_surface_view =
						webgpu_platform_backbuffer_view(g_device);
					if (!g_surface_view)
						return;
				}
			}
			color[i].view = g_surface_view;
			if (i == 0) {
				c->pass_w = g_surface_w;
				c->pass_h = g_surface_h;
			}
		} else {
			struct gpu_texture *t = (struct gpu_texture *)a->texture;

			if (!t->view)
				return;
			color[i].view = t->view;
			if (i == 0) {
				c->pass_w = t->width;
				c->pass_h = t->height;
			}
		}

		/*
		 * MSAA resolve: when the attachment names a single-sample
		 * resolve target, WebGPU resolves this (multisampled) colour into
		 * it as the pass ends — the multisampled-to-single-sample step the
		 * post passes need, since they sample the resolved texture. NULL
		 * (every single-sample pass) leaves it unset, exactly as before.
		 */
		if (a->resolve_target) {
			struct gpu_texture *rt =
				(struct gpu_texture *)a->resolve_target;

			color[i].resolveTarget = rt->view;
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

	memset(&rp, 0, sizeof(rp));
	rp.colorAttachmentCount = count;
	rp.colorAttachments     = count ? color : NULL;

	/*
	 * GL's default framebuffer comes with a depth buffer, so a pass drawing
	 * straight to the backbuffer never had to declare one — the scene
	 * renderer's forward-to-backbuffer path (every shipped game, and any
	 * frame with nothing selected) relies on exactly that. A WebGPU surface
	 * texture has no such companion, so the backend keeps one sized to the
	 * surface and attaches it here. That is emulating a real property of the
	 * GL backbuffer rather than papering over a caller's mistake, which is
	 * why it belongs at this layer: the backbuffer is the backend's to
	 * describe.
	 *
	 * It clears every pass because the frame graph only fills in depth load
	 * ops for a declared depth write, and a pass that never declared one
	 * would otherwise inherit last frame's depth.
	 *
	 * Scoped to passes that actually target the backbuffer. Keying off "a
	 * surface view exists" instead attached it to every depthless OFFSCREEN
	 * pass too, once anything acquired the surface earlier in the frame —
	 * which handed the four half-res bloom passes a surface-sized depth
	 * buffer their pipelines never declared, and every draw in them failed
	 * with "Attachment state ... is not compatible with [RenderPass]".
	 */
	if (!desc->depth && targets_backbuffer && g_depth) {
		struct gpu_texture *d = (struct gpu_texture *)g_depth;

		memset(&depth_att, 0, sizeof(depth_att));
		depth_att.view            = d->view;
		depth_att.depthLoadOp     = WGPULoadOp_Clear;
		depth_att.depthStoreOp    = WGPUStoreOp_Store;
		depth_att.depthClearValue = 1.0f;
		depth_att.stencilLoadOp   = WGPULoadOp_Undefined;
		depth_att.stencilStoreOp  = WGPUStoreOp_Undefined;
		rp.depthStencilAttachment = &depth_att;
	} else if (desc->depth) {
		struct gpu_texture *d = (struct gpu_texture *)desc->depth;

		memset(&depth_att, 0, sizeof(depth_att));
		depth_att.view             = d->view;
		depth_att.depthLoadOp      = to_load_op(desc->depth_load_op);
		depth_att.depthStoreOp     = to_store_op(desc->depth_store_op);
		depth_att.depthClearValue  = desc->clear_depth;
		/*
		 * Depth32Float carries no stencil, and WebGPU rejects stencil ops
		 * on a format without one, so they stay Undefined rather than
		 * mirroring the depth ops.
		 */
		depth_att.stencilLoadOp    = WGPULoadOp_Undefined;
		depth_att.stencilStoreOp   = WGPUStoreOp_Undefined;
		rp.depthStencilAttachment  = &depth_att;
	}

	/* A depth-only pass (the sun shadow map) has no colour attachment to
	 * take the scissor bounds from, so they come from its depth target. */
	if (count == 0 && desc->depth) {
		struct gpu_texture *d = (struct gpu_texture *)desc->depth;

		c->pass_w = d->width;
		c->pass_h = d->height;
	}

	c->pass = wgpuCommandEncoderBeginRenderPass(c->enc, &rp);

	/*
	 * A render pass encoder starts with NO pipeline and NO bind groups: that
	 * state is pass-scoped in WebGPU, not frame-scoped. The dirty flags exist
	 * to skip redundant SetBindGroup calls *within* a pass, so they have to be
	 * re-armed here — otherwise a second pass that reuses the previous pass's
	 * pipeline and bindings sees a clean flag, never calls SetBindGroup, and
	 * every draw in it fails with "No bind group set at group index 0".
	 *
	 * This was latent until bloom (#622) put four passes in a frame with
	 * blur-H and blur-V sharing one pipeline back to back; before that the
	 * frame was effectively a single pass and the flags happened to be safe.
	 */
	c->pipeline       = NULL;
	c->bindings_dirty = 1;
	c->textures_dirty = 1;
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
 * Record a uniform binding. Nothing reaches the GPU here: WebGPU has no mutable
 * slot state a later draw could inherit, so the binding is staged and the draw
 * resolves the whole set into an immutable bind group.
 */
static void webgpu_cmd_bind_uniform_buffer(gpu_cmd_buf_t cmd, uint32_t slot,
					   gpu_buffer_t buf, uint32_t offset,
					   uint32_t size)
{
	struct gpu_cmd_buf_state *c = (struct gpu_cmd_buf_state *)cmd;
	struct gpu_buffer *b = (struct gpu_buffer *)buf;
	struct uniform_binding *u;

	if (!c || slot >= WGPU_MAX_UNIFORM_SLOTS)
		return;

	u = &c->uniforms[slot];
	if (!b || !b->buf) {
		if (u->buf) {
			memset(u, 0, sizeof(*u));
			c->uniform_mask &= ~(1u << slot);
			c->bindings_dirty = 1;
		}
		return;
	}

	if (u->buf == b->buf && u->offset == offset && u->size == size)
		return;

	u->buf    = b->buf;
	u->offset = offset;
	u->size   = size ? size : b->size - offset;
	c->uniform_mask |= 1u << slot;
	c->bindings_dirty = 1;
}

/* --------------------------------------------------------------- draws */

static void webgpu_cmd_draw(gpu_cmd_buf_t cmd, uint32_t vertex_count,
			    uint32_t instance_count, uint32_t first_vertex,
			    uint32_t first_instance)
{
	struct gpu_cmd_buf_state *c = (struct gpu_cmd_buf_state *)cmd;

	if (!c || !c->pass)
		return;
	if (!apply_bindings(c) || !apply_textures(c))
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
	if (!apply_bindings(c) || !apply_textures(c))
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

/*
 * Two things separate this from the WebGL backend's one-line glScissor.
 *
 * The gpu_api's scissor box is GL's — origin bottom-left, y increasing upward
 * — and callers flip into it themselves (kruddgui computes
 * `s_phys_h - (y + h) * scale`). WebGPU's box is top-left with y increasing
 * downward, so it is mirrored about the attachment height here. Doing it in the
 * backend rather than retargeting the contract keeps the WebGL backend and every
 * existing caller untouched, which is what makes this a non-breaking change.
 *
 * And WebGPU *validates* the box against the attachment, failing the whole
 * command buffer if it escapes; GL simply clipped. A caller rounding a CSS rect
 * up to physical pixels can overhang the target by one, so the box is clamped
 * into range. A rect clipped away to nothing becomes an empty scissor, which
 * draws nothing — the same outcome GL gave it.
 */
static void webgpu_cmd_set_scissor(gpu_cmd_buf_t cmd, int32_t x, int32_t y,
				   uint32_t width, uint32_t height)
{
	struct gpu_cmd_buf_state *c = (struct gpu_cmd_buf_state *)cmd;
	int64_t tw, th, x0, y0, x1, y1;

	if (!c || !c->pass || !c->pass_w || !c->pass_h)
		return;

	tw = (int64_t)c->pass_w;
	th = (int64_t)c->pass_h;

	x0 = (int64_t)x;
	x1 = x0 + (int64_t)width;
	/* GL y-up -> WebGPU y-down: the box's top edge is the target height
	 * less its GL top (y + height). */
	y0 = th - ((int64_t)y + (int64_t)height);
	y1 = y0 + (int64_t)height;

	if (x0 < 0)  x0 = 0;
	if (y0 < 0)  y0 = 0;
	if (x1 > tw) x1 = tw;
	if (y1 > th) y1 = th;
	if (x1 < x0) x1 = x0;
	if (y1 < y0) y1 = y0;

	wgpuRenderPassEncoderSetScissorRect(c->pass, (uint32_t)x0, (uint32_t)y0,
					    (uint32_t)(x1 - x0),
					    (uint32_t)(y1 - y0));
}

/* --------------------------------------------------------- mip chain */

/*
 * WebGL builds mip chains through glGenerateMipmap, which allocates every
 * level itself; WebGPU textures are immutable once created, so a texture that
 * wants mips has to declare the full chain up front, and something has to
 * fill levels 1..N-1 in explicitly. This is that "something": a tiny
 * fullscreen-quad blit pipeline, hand-authored in WGSL rather than the krudd
 * DSL because it samples one specific mip level of a texture — a per-level
 * WGPUTextureView the shader DSL's binding model has no way to express — and
 * runs entirely inside this file rather than through the generic gpu_api
 * command-recording path, since it happens at texture_create() before any
 * frame's command buffer exists (mirroring the immediate, synchronous
 * wgpuQueueWriteTexture already used for level 0's initial data, a few lines
 * below).
 *
 * Downsampling is a single bilinear tap per destination texel: rendering a
 * full [0,1] UV quad into a target at half the source's resolution lands
 * each destination texel's sample point exactly between four source texels,
 * so the hardware's linear filter is doing a 2x2 box filter for free. No
 * sampling ever lands outside [0,1], so the shared repeat-wrap sampler
 * (g_sampler) is safe to reuse here.
 */
static const char *MIP_BLIT_WGSL =
	"struct VOut {\n"
	"  @builtin(position) position : vec4<f32>,\n"
	"  @location(0) uv : vec2<f32>,\n"
	"};\n"
	"@vertex\n"
	"fn vs_main(@location(0) pos : vec2<f32>, @location(1) uv : vec2<f32>) -> VOut {\n"
	"  var out : VOut;\n"
	"  out.position = vec4<f32>(pos, 0.0, 1.0);\n"
	"  out.uv = uv;\n"
	"  return out;\n"
	"}\n"
	"@group(0) @binding(0) var mip_sampler : sampler;\n"
	"@group(0) @binding(1) var mip_src : texture_2d<f32>;\n"
	"@fragment\n"
	"fn fs_main(in : VOut) -> @location(0) vec4<f32> {\n"
	"  return textureSample(mip_src, mip_sampler, in.uv);\n"
	"}\n";

/* A clip-space quad (two triangles) with matching [0,1] UVs; see the comment
 * above MIP_BLIT_WGSL for why a plain full-quad sample is the right filter. */
static const float MIP_BLIT_VERTS[] = {
	/* pos.xy          uv.xy */
	-1.0f,  1.0f,      0.0f, 0.0f, /* top-left */
	-1.0f, -1.0f,      0.0f, 1.0f, /* bottom-left */
	 1.0f,  1.0f,      1.0f, 0.0f, /* top-right */
	 1.0f,  1.0f,      1.0f, 0.0f, /* top-right */
	-1.0f, -1.0f,      0.0f, 1.0f, /* bottom-left */
	 1.0f, -1.0f,      1.0f, 1.0f, /* bottom-right */
};

/* Full mip chain length for a 2D extent, matching WebGL's implicit
 * floor(log2(max(w,h))) + 1 (glGenerateMipmap's rule). */
static uint32_t mip_chain_length(uint32_t w, uint32_t h)
{
	uint32_t levels = 1;
	uint32_t m = w > h ? w : h;

	while (m > 1) {
		m >>= 1;
		levels++;
	}
	return levels;
}

static void ensure_mip_blit_resources(void)
{
	WGPUShaderModule mod;
	WGPUVertexAttribute attrs[2];
	WGPUVertexBufferLayout vblayout;
	WGPUColorTargetState target;
	WGPUFragmentState frag;
	WGPURenderPipelineDescriptor pd;
	WGPUBufferDescriptor bd;

	if (g_mip_pso)
		return;

	mod = make_module(MIP_BLIT_WGSL);
	if (!mod) {
		g_log->write(LOG_LEVEL_ERROR,
			     "renderer_webgpu: mip blit shader module failed");
		return;
	}

	memset(attrs, 0, sizeof(attrs));
	attrs[0].format         = WGPUVertexFormat_Float32x2;
	attrs[0].offset         = 0;
	attrs[0].shaderLocation = 0;
	attrs[1].format         = WGPUVertexFormat_Float32x2;
	attrs[1].offset         = 2 * sizeof(float);
	attrs[1].shaderLocation = 1;

	memset(&vblayout, 0, sizeof(vblayout));
	vblayout.arrayStride    = 4 * sizeof(float);
	vblayout.stepMode       = WGPUVertexStepMode_Vertex;
	vblayout.attributeCount = 2;
	vblayout.attributes     = attrs;

	memset(&target, 0, sizeof(target));
	/* Every mip-chain caller today bakes RGBA8 procedural textures (see the
	 * GL backend's texture_create); this pipeline is built once and reused,
	 * so it is pinned to that one format rather than re-derived per call. */
	target.format    = WGPUTextureFormat_RGBA8Unorm;
	target.writeMask = WGPUColorWriteMask_All;

	memset(&frag, 0, sizeof(frag));
	frag.module     = mod;
	frag.entryPoint = str_view("fs_main");
	frag.targetCount = 1;
	frag.targets     = &target;

	memset(&pd, 0, sizeof(pd));
	pd.layout                = NULL; /* auto layout from the WGSL above */
	pd.vertex.module         = mod;
	pd.vertex.entryPoint     = str_view("vs_main");
	pd.vertex.bufferCount    = 1;
	pd.vertex.buffers        = &vblayout;
	pd.primitive.topology    = WGPUPrimitiveTopology_TriangleList;
	pd.multisample.count     = 1;
	pd.multisample.mask      = 0xFFFFFFFFu;
	pd.fragment              = &frag;

	g_mip_pso = wgpuDeviceCreateRenderPipeline(g_device, &pd);
	wgpuShaderModuleRelease(mod);
	if (!g_mip_pso) {
		g_log->write(LOG_LEVEL_ERROR,
			     "renderer_webgpu: mip blit pipeline creation failed");
		return;
	}

	memset(&bd, 0, sizeof(bd));
	bd.size  = sizeof(MIP_BLIT_VERTS);
	bd.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
	g_mip_vbo = wgpuDeviceCreateBuffer(g_device, &bd);
	if (g_mip_vbo)
		wgpuQueueWriteBuffer(g_queue, g_mip_vbo, 0, MIP_BLIT_VERTS,
				     sizeof(MIP_BLIT_VERTS));
}

/*
 * Fill mip levels 1..t->mip_levels-1 by successively blitting each level into
 * the next, half-res each step. Every step is its own tiny command buffer,
 * submitted synchronously — texture_create has no frame command buffer to
 * record into, and this runs once per baked texture rather than per frame.
 */
static void generate_mip_chain(struct gpu_texture *t)
{
	uint32_t level;

	ensure_mip_blit_resources();
	if (!g_mip_pso || !g_mip_vbo)
		return;

	for (level = 0; level + 1 < t->mip_levels; level++) {
		WGPUTextureViewDescriptor vd;
		WGPUTextureView       src_view, dst_view;
		WGPUBindGroupLayout   bgl;
		WGPUBindGroupEntry    entries[2];
		WGPUBindGroupDescriptor bgd;
		WGPUBindGroup         bg;
		WGPURenderPassColorAttachment att;
		WGPURenderPassDescriptor rpd;
		WGPUCommandEncoder    enc;
		WGPURenderPassEncoder pass;
		WGPUCommandBuffer     cmdbuf;

		memset(&vd, 0, sizeof(vd));
		vd.format          = WGPUTextureFormat_RGBA8Unorm;
		vd.baseMipLevel    = level;
		vd.mipLevelCount   = 1;
		vd.baseArrayLayer  = 0;
		vd.arrayLayerCount = 1;
		vd.dimension       = WGPUTextureViewDimension_2D;
		vd.aspect          = WGPUTextureAspect_All;
		src_view = wgpuTextureCreateView(t->tex, &vd);

		vd.baseMipLevel = level + 1;
		dst_view = wgpuTextureCreateView(t->tex, &vd);

		if (!src_view || !dst_view) {
			g_log->write(LOG_LEVEL_ERROR,
				     "renderer_webgpu: mip level %u view failed",
				     level + 1);
			if (src_view) wgpuTextureViewRelease(src_view);
			if (dst_view) wgpuTextureViewRelease(dst_view);
			break;
		}

		bgl = wgpuRenderPipelineGetBindGroupLayout(g_mip_pso, 0);
		memset(entries, 0, sizeof(entries));
		entries[0].binding = 0;
		entries[0].sampler = g_sampler;
		entries[1].binding = 1;
		entries[1].textureView = src_view;
		memset(&bgd, 0, sizeof(bgd));
		bgd.layout     = bgl;
		bgd.entryCount = 2;
		bgd.entries    = entries;
		bg = wgpuDeviceCreateBindGroup(g_device, &bgd);
		wgpuBindGroupLayoutRelease(bgl);

		memset(&att, 0, sizeof(att));
		att.view          = dst_view;
		att.depthSlice    = WGPU_DEPTH_SLICE_UNDEFINED;
		att.loadOp        = WGPULoadOp_Clear;
		att.storeOp       = WGPUStoreOp_Store;
		att.clearValue.r  = 0.0;
		att.clearValue.g  = 0.0;
		att.clearValue.b  = 0.0;
		att.clearValue.a  = 0.0;

		memset(&rpd, 0, sizeof(rpd));
		rpd.colorAttachmentCount = 1;
		rpd.colorAttachments     = &att;

		enc  = wgpuDeviceCreateCommandEncoder(g_device, NULL);
		pass = wgpuCommandEncoderBeginRenderPass(enc, &rpd);
		if (bg) {
			wgpuRenderPassEncoderSetPipeline(pass, g_mip_pso);
			wgpuRenderPassEncoderSetBindGroup(pass, 0, bg, 0, NULL);
			wgpuRenderPassEncoderSetVertexBuffer(
				pass, 0, g_mip_vbo, 0, sizeof(MIP_BLIT_VERTS));
			wgpuRenderPassEncoderDraw(pass, 6, 1, 0, 0);
		}
		wgpuRenderPassEncoderEnd(pass);
		wgpuRenderPassEncoderRelease(pass);

		cmdbuf = wgpuCommandEncoderFinish(enc, NULL);
		wgpuQueueSubmit(g_queue, 1, &cmdbuf);
		wgpuCommandBufferRelease(cmdbuf);
		wgpuCommandEncoderRelease(enc);

		if (bg)
			wgpuBindGroupRelease(bg);
		wgpuTextureViewRelease(src_view);
		wgpuTextureViewRelease(dst_view);
	}
}

static gpu_texture_t webgpu_texture_create(const struct gpu_texture_desc *desc)
{
	struct gpu_texture *t;
	WGPUTextureDescriptor td;

	if (!g_ready)
		return NULL;

	t = g_mem->alloc(sizeof(*t));
	if (!t)
		return NULL;
	memset(t, 0, sizeof(*t));

	t->width      = desc->width;
	t->height     = desc->height;
	t->format     = desc->format;
	t->mip_levels = desc->mip_levels ? desc->mip_levels : 1;
	/*
	 * A caller asking for generate_mips typically still describes only level
	 * 0 (see scene_renderer.c's ensure_textures, which sets mip_levels = 1
	 * alongside generate_mips = 1) — that is enough for GL, where
	 * glGenerateMipmap allocates the rest of the chain itself. WebGPU
	 * textures are immutable once created, so the full chain has to be sized
	 * here, before wgpuDeviceCreateTexture, or there is nowhere for
	 * generate_mip_chain to blit into.
	 */
	if (desc->generate_mips && t->mip_levels <= 1)
		t->mip_levels = mip_chain_length(desc->width, desc->height);

	memset(&td, 0, sizeof(td));
	td.dimension     = WGPUTextureDimension_2D;
	td.size.width    = desc->width;
	td.size.height   = desc->height;
	td.size.depthOrArrayLayers = 1;
	td.format        = to_texture_format(desc->format);
	td.mipLevelCount = t->mip_levels;
	td.sampleCount   = desc->sample_count ? desc->sample_count : 1;
	/*
	 * Every texture the engine makes is either drawn into or sampled, and
	 * usually both (a shadow map is a depth target this frame and a sampled
	 * texture the next), so ask for both up front rather than guessing from
	 * the descriptor. CopyDst covers pixel uploads.
	 *
	 * A multisampled texture is the exception: WebGPU forbids CopyDst on one,
	 * and the engine never samples the multisampled colour/depth directly
	 * (it resolves to a single-sample texture and samples that), so a
	 * multisampled target is a render attachment only.
	 */
	if (td.sampleCount > 1)
		td.usage = WGPUTextureUsage_RenderAttachment;
	else
		td.usage = WGPUTextureUsage_RenderAttachment |
			   WGPUTextureUsage_TextureBinding |
			   WGPUTextureUsage_CopyDst;

	t->tex = wgpuDeviceCreateTexture(g_device, &td);
	if (!t->tex) {
		g_log->write(LOG_LEVEL_ERROR,
			     "renderer_webgpu: texture_create failed (%ux%u fmt=%d)",
			     desc->width, desc->height, (int)desc->format);
		g_mem->free(t);
		return NULL;
	}
	t->view = wgpuTextureCreateView(t->tex, NULL);

	if (desc->initial_data) {
		WGPUTexelCopyTextureInfo dst;
		WGPUTexelCopyBufferLayout layout;
		WGPUExtent3D extent;
		uint32_t bpp = (desc->format == GPU_FORMAT_RGBA32_FLOAT) ? 16u : 4u;

		memset(&dst, 0, sizeof(dst));
		dst.texture  = t->tex;
		dst.mipLevel = 0;
		dst.aspect   = WGPUTextureAspect_All;

		memset(&layout, 0, sizeof(layout));
		layout.bytesPerRow  = desc->width * bpp;
		layout.rowsPerImage = desc->height;

		extent.width              = desc->width;
		extent.height             = desc->height;
		extent.depthOrArrayLayers = 1;

		wgpuQueueWriteTexture(g_queue, &dst, desc->initial_data,
				      (size_t)desc->width * desc->height * bpp,
				      &layout, &extent);
	}
	if (desc->generate_mips) {
		if (desc->format == GPU_FORMAT_RGBA8_UNORM) {
			generate_mip_chain(t);
		} else {
			/* The blit pipeline is pinned to RGBA8Unorm (see
			 * ensure_mip_blit_resources) because it is the engine's
			 * only sampled/color-target format today; a caller
			 * asking for mips on anything else needs that pipeline
			 * generalized first. */
			WARN_ONCE("generate_mips only supports RGBA8_UNORM textures");
		}
	}

	return (gpu_texture_t)t;
}

static void release_texture(struct gpu_texture *t)
{
	if (t->view)
		wgpuTextureViewRelease(t->view);
	if (t->tex) {
		wgpuTextureDestroy(t->tex);
		wgpuTextureRelease(t->tex);
	}
	g_mem->free(t);
}

/*
 * The frame graph frees a transient the moment its last reader ends, which is
 * still mid-frame -- the command buffer holding those draws is not submitted
 * until every pass has run. GL tolerates that; WebGPU rejects the submit
 * outright ("Destroyed texture used in a submit"), because a destroy there is
 * immediate rather than refcounted against pending work.
 *
 * Rather than push that difference up into fg.c, where it would complicate a
 * contract that is correct as written, destruction is deferred here until the
 * submit those commands belong to has gone through.
 */
static void webgpu_texture_destroy(gpu_texture_t texture)
{
	struct gpu_texture *t = (struct gpu_texture *)texture;

	if (!t)
		return;
	g_texture_generation++;
	/*
	 * Forget it here rather than at the deferred release: the caller has said
	 * the texture is done with, so its ids must stop resolving now. Leaving
	 * them live until the flush would let a draw later this frame sample a
	 * texture the engine considers destroyed.
	 */
	texreg_forget(t);

	if (g_pending_destroy_count < WGPU_MAX_PENDING_DESTROY) {
		g_pending_destroy[g_pending_destroy_count++] = t;
		return;
	}

	/*
	 * More transients died in one frame than the queue holds. Destroying
	 * now is what the caller asked for and keeps the leak from growing; if
	 * the texture is live in this frame's commands the submit will say so.
	 */
	g_log->write(LOG_LEVEL_ERROR,
		     "renderer_webgpu: pending-destroy queue full (%u); destroying immediately",
		     (unsigned)WGPU_MAX_PENDING_DESTROY);
	release_texture(t);
}

static void flush_pending_destroys(void)
{
	uint32_t i;

	for (i = 0; i < g_pending_destroy_count; i++)
		release_texture(g_pending_destroy[i]);
	g_pending_destroy_count = 0;
}

static void webgpu_cmd_bind_texture(gpu_cmd_buf_t cmd, uint32_t unit,
				    gpu_texture_t texture)
{
	struct gpu_cmd_buf_state *c = (struct gpu_cmd_buf_state *)cmd;
	struct gpu_texture *t = (struct gpu_texture *)texture;
	WGPUTextureView view = t ? t->view : NULL;

	if (!c || unit >= WGPU_MAX_TEXTURE_UNITS)
		return;
	if (c->textures[unit] == view)
		return;
	c->textures[unit] = view;
	/*
	 * Which sampler the bind group needs is a property of the texture, not
	 * of the view, so it has to be recorded here while the format is still
	 * in hand -- the group builder sees only views.
	 */
	c->texture_depth[unit] = t && t->format == GPU_FORMAT_DEPTH32_FLOAT;
	c->textures_dirty = 1;
}

/*
 * WebGPU has no integer that names a texture — a WGPUTexture is a pointer, and
 * the id has to survive a trip through Scheme as a number. So the backend hands
 * out ids from texture_registry, whose id algebra (and, more to the point, its
 * stale-id behaviour) is specified and tested there.
 */
static uint32_t webgpu_texture_handle(gpu_texture_t texture)
{
	uint32_t handle;

	if (!texture)
		return 0;

	handle = texreg_intern(texture);
	if (!handle)
		g_log->write(LOG_LEVEL_ERROR,
			     "renderer_webgpu: texture handle registry full (%d)",
			     TEXREG_CAPACITY);
	return handle;
}

/*
 * Bind by id. Resolving to NULL is not an error: an id whose texture has been
 * destroyed unbinds the unit, which the vtable specifies and which a UI holding
 * a stale id across a rebake relies on.
 */
static void webgpu_cmd_bind_texture_handle(gpu_cmd_buf_t cmd, uint32_t unit,
					   uint32_t handle)
{
	webgpu_cmd_bind_texture(cmd, unit, (gpu_texture_t)texreg_resolve(handle));
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
	/* Draws yes; compute is not wired up in this backend. WebGPU clips depth
	 * to NDC z in [0, 1], so it advertises the clip-z convention the scene
	 * layer adapts GL-built projections to, and it resolves a multisampled
	 * colour target to a single-sample texture in-pass (a render pass's
	 * resolveTarget), so it advertises MSAA resolve. */
	.caps                    = GPU_CAP_DRAW_DIRECT | GPU_CAP_DRAW_INDEXED
				 | GPU_CAP_CLIP_Z_ZERO_TO_ONE
				 | GPU_CAP_MSAA_RESOLVE,
	.cmd_buf_begin           = webgpu_cmd_buf_begin,
	.cmd_buf_submit          = webgpu_cmd_buf_submit,
	.frame_end               = webgpu_frame_end,
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
	.cmd_dispatch            = NULL, /* no compute in this backend */
	.gpu_malloc              = webgpu_gpu_malloc,
	.gpu_free                = webgpu_gpu_free,
	.gpu_host_to_device_ptr  = NULL, /* bindless only */
	.texture_create          = webgpu_texture_create,
	.texture_destroy         = webgpu_texture_destroy,
	.cmd_bind_texture        = webgpu_cmd_bind_texture,
	.texture_handle          = webgpu_texture_handle,
	.cmd_bind_texture_handle = webgpu_cmd_bind_texture_handle,
};

/* ------------------------------------------------- surface and device */

/*
 * Match the surface to the canvas and the device's preferred format. Called
 * once the device arrives; a resize hook comes with the full backend.
 */
static void configure_surface(void)
{
	uint32_t bw = 0;
	uint32_t bh = 0;
	int w;
	int h;
	WGPUSurfaceCapabilities caps;
	WGPUSurfaceConfiguration cfg;

	webgpu_platform_backbuffer_size(&bw, &bh);
	w = (int)bw;
	h = (int)bh;
	g_surface_w = bw;
	g_surface_h = bh;

	/*
	 * No surface means this platform renders offscreen (native), so there is
	 * nothing to configure and no capabilities to negotiate. Pick the format
	 * the scene renderer builds its pipelines for and stop — the frame's
	 * colour target is created as an ordinary texture instead.
	 */
	if (!g_surface) {
		char buf[96];

		g_format = WGPUTextureFormat_RGBA8Unorm;
		snprintf(buf, sizeof(buf),
			 "webgpu: offscreen %dx%d format=%d",
			 w, h, (int)g_format);
		webgpu_status(buf);
		return;
	}

	memset(&caps, 0, sizeof(caps));
	wgpuSurfaceGetCapabilities(g_surface, g_adapter, &caps);
	/*
	 * Prefer RGBA8Unorm when the surface supports it. WebGPU validates a
	 * pipeline's colour formats against the pass it draws into, and the scene
	 * renderer builds its pipelines for RGBA8Unorm; taking the surface's
	 * preferred BGRA8Unorm instead would make every pipeline that targets the
	 * backbuffer fail validation. GL had no such coupling, which is why the
	 * scene renderer could hardcode a format at all.
	 */
	g_format = (caps.formatCount > 0) ? caps.formats[0]
					  : WGPUTextureFormat_BGRA8Unorm;
	{
		size_t fi;

		for (fi = 0; fi < caps.formatCount; fi++) {
			if (caps.formats[fi] == WGPUTextureFormat_RGBA8Unorm) {
				g_format = WGPUTextureFormat_RGBA8Unorm;
				break;
			}
		}
	}

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
 * (Re)build the backbuffer's companion depth target at the given size. The pass
 * path calls this the moment it acquires a surface texture, so g_depth always
 * matches the current backbuffer for the fallback attachment in begin_render_pass.
 */
static void ensure_depth_target(uint32_t w, uint32_t h)
{
	struct gpu_texture_desc td;

	if (w < 1 || h < 1)
		return;
	if (g_depth && g_depth_w == w && g_depth_h == h)
		return;

	if (g_depth)
		webgpu_api.texture_destroy(g_depth);

	memset(&td, 0, sizeof(td));
	td.format       = GPU_FORMAT_DEPTH32_FLOAT;
	td.width        = w;
	td.height       = h;
	td.mip_levels   = 1;
	td.sample_count = 1;

	g_depth   = webgpu_api.texture_create(&td);
	g_depth_w = td.width;
	g_depth_h = td.height;
}

/*
 * Boot-time sizing, before any surface texture has been acquired. From the
 * first acquire onward the pass path re-sizes against the real backbuffer.
 */
static void create_depth_target(void)
{
	uint32_t w = 0, h = 0;

	webgpu_platform_backbuffer_size(&w, &h);
	if (w < 1 || h < 1)
		return;
	g_surface_w = w;
	g_surface_h = h;
	ensure_depth_target(w, h);
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

	/* The device is live: create the samplers every pipeline binds. */
	{
		WGPUSamplerDescriptor sd;

		memset(&sd, 0, sizeof(sd));
		sd.addressModeU = WGPUAddressMode_Repeat;
		sd.addressModeV = WGPUAddressMode_Repeat;
		sd.addressModeW = WGPUAddressMode_Repeat;
		sd.magFilter    = WGPUFilterMode_Linear;
		sd.minFilter    = WGPUFilterMode_Linear;
		sd.mipmapFilter = WGPUMipmapFilterMode_Linear;
		sd.lodMaxClamp  = 32.0f;
		sd.maxAnisotropy = 1;
		g_sampler = wgpuDeviceCreateSampler(g_device, &sd);

		/*
		 * A depth texture lowers to texture_depth_2d, whose derived
		 * layout asks for a non-filtering sampler -- binding the linear
		 * one above is a validation error. GL has no such split (the
		 * shadow shaders do their own 3x3 PCF with step, so nothing is
		 * asking the hardware to filter anyway), which is why this
		 * second sampler exists only here.
		 */
		sd.magFilter    = WGPUFilterMode_Nearest;
		sd.minFilter    = WGPUFilterMode_Nearest;
		sd.mipmapFilter = WGPUMipmapFilterMode_Nearest;
		g_depth_sampler = wgpuDeviceCreateSampler(g_device, &sd);
	}

	g_ready = 1;
	create_depth_target();

	webgpu_announce_renderer();
	webgpu_status("webgpu: device ready");
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
	WGPURequestAdapterCallbackInfo ci;

	g_instance = wgpuCreateInstance(NULL);
	if (!g_instance) {
		webgpu_status("webgpu: no instance");
		g_log->write(LOG_LEVEL_ERROR, "renderer_webgpu: no instance");
		return;
	}
	webgpu_status("webgpu: requesting adapter");

	/* NULL here is the offscreen platform's answer, not a failure — see
	 * webgpu_platform.h and the configure_surface offscreen path. */
	g_surface = webgpu_platform_create_surface(g_instance);

	memset(&ci, 0, sizeof(ci));
	ci.mode     = WGPUCallbackMode_AllowSpontaneous;
	ci.callback = on_adapter;
	wgpuInstanceRequestAdapter(g_instance, NULL, ci);

	g_log->write(LOG_LEVEL_INFO,
		     "renderer_webgpu: init (requesting adapter)");
}

/*
 * Pump the instance so the async adapter/device futures resolve. Before the
 * device lands the backend is the only registered subsystem and this is the one
 * thing that has to run each frame; once the render cluster boots the scene
 * renderer owns the frame and this keeps pumping harmlessly beside it.
 * AllowSpontaneous callbacks should fire on their own, but processing events
 * each frame covers backends that need the nudge.
 */
static void renderer_webgpu_tick(void)
{
	if (g_instance)
		wgpuInstanceProcessEvents(g_instance);
}

static void renderer_webgpu_shutdown(void)
{
	/*
	 * texture_destroy defers onto g_pending_destroy (a frame's commands may
	 * still name the texture); at shutdown no further submit flushes that
	 * queue, so drain it by hand or the texture and its view leak.
	 */
	if (g_depth)
		webgpu_api.texture_destroy(g_depth);
	g_depth = NULL;
	flush_pending_destroys();
	texreg_reset();

	if (g_mip_vbo)
		wgpuBufferRelease(g_mip_vbo);
	if (g_mip_pso)
		wgpuRenderPipelineRelease(g_mip_pso);
	if (g_sampler)
		wgpuSamplerRelease(g_sampler);
	if (g_depth_sampler)
		wgpuSamplerRelease(g_depth_sampler);
	g_mip_vbo       = NULL;
	g_mip_pso       = NULL;
	g_sampler       = NULL;
	g_depth_sampler = NULL;

	/* The platform owns the offscreen backbuffer (native only); it must go
	 * before the device that created it. */
	webgpu_platform_teardown();

	/*
	 * The surface goes before the device. A windowed surface still owns a
	 * swapchain, and ~Surface detaches it — which reaches into the device's
	 * FencedDeleter to retire the swapchain's fences. Release the device
	 * first and that deleter is already gone, so the detach faults. The
	 * offscreen path never sees this: g_surface is NULL there.
	 */
	if (g_surface)
		wgpuSurfaceRelease(g_surface);
	if (g_queue)
		wgpuQueueRelease(g_queue);
	if (g_device)
		wgpuDeviceRelease(g_device);
	if (g_adapter)
		wgpuAdapterRelease(g_adapter);
	if (g_instance)
		wgpuInstanceRelease(g_instance);
	g_queue    = NULL;
	g_device   = NULL;
	g_adapter  = NULL;
	g_surface  = NULL;
	g_instance = NULL;

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

/*
 * The render cluster is about to own the backbuffer. engine.c calls this as it
 * finishes the deferred boot, just before it registers the frame graph and
 * scene renderer; it is the boot milestone that marks the backend handing the
 * frame off from bring-up to the real render path.
 */
void renderer_webgpu_release_frame(void)
{
	webgpu_status("webgpu: render cluster live");
}

/*
 * Whether the device has arrived and the vtable is usable. engine.c gates the
 * rest of the boot on this: plugins create GPU resources in their init, and
 * none of that can run before there is a device to create them on.
 */
int renderer_webgpu_device_ready(void)
{
	return g_ready;
}

/*
 * Copy the frame's colour target out into `rgba` (w*h*4, tightly packed). Only
 * an offscreen platform can answer this — on the web the backbuffer is the
 * canvas's and belongs to the compositor — so this returns 0 there, and 0 before
 * the device has arrived. It is the whole point of the native target: the
 * picture the engine actually produced, in bytes, with no compositor in between.
 */
int renderer_webgpu_read_backbuffer(uint8_t *rgba)
{
	if (!g_ready)
		return 0;
	return webgpu_platform_read_backbuffer(g_instance, g_device, g_queue,
					       rgba);
}
