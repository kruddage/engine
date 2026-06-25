/* SPDX-License-Identifier: LGPL-2.1-or-later */

/*
 * renderer_webgpu — WebGPU backend for renderer.h
 *
 * =========================================================================
 * AALTONEN → WEBGPU MAPPING
 * (Document all design decisions before any implementation is written.)
 * =========================================================================
 *
 * Reference: Sebastian Aaltonen — "No Graphics API"
 * https://www.sebastianaaltonen.com/blog/no-graphics-api
 *
 * WebGPU differs from the Aaltonen / Vulkan mental model in three
 * fundamental ways. Every mismatch in our renderer.h is traced to one of:
 *
 *   A. BARRIERS          — WebGPU has no explicit barrier API; sync is
 *                          inferred automatically from usage flags and
 *                          render pass begin/end.
 *
 *   B. ROOT DATA POINTER — Aaltonen's draw calls take a raw void* GPU
 *                          pointer ("the shader receives it as
 *                          const RootData*"). WebGPU has no GPU virtual
 *                          addresses. Data must live in a WGPUBuffer and
 *                          be bound via a WGPUBindGroup.
 *
 *   C. PERSISTENT MAPS   — Aaltonen's gpu_malloc returns CPU-mapped GPU
 *                          memory that stays mapped. WebGPU has no
 *                          persistent mapping. Writes go through
 *                          wgpuQueueWriteBuffer (a staging copy).
 *
 * Each gpu_api function is documented below with the exact WebGPU calls
 * it expands to and which mismatch class (A/B/C) applies, if any.
 *
 * -------------------------------------------------------------------------
 * INTERNAL TYPES
 * -------------------------------------------------------------------------
 *
 * WebGPU uses a two-level command structure — a command encoder plus an
 * active render/compute pass encoder. Our single gpu_cmd_buf_t handle
 * carries both:
 *
 *   struct webgpu_cmd_buf {
 *       WGPUCommandEncoder    encoder;
 *       WGPURenderPassEncoder render_pass;  // NULL when not in a pass
 *   };
 *   typedef struct webgpu_cmd_buf *gpu_cmd_buf_t;
 *
 * gpu_pipeline_t wraps WGPURenderPipeline directly (cast through void*).
 *
 * gpu_texture_t wraps a struct holding WGPUTexture + WGPUTextureView (the
 * view is needed for render pass attachments):
 *
 *   struct webgpu_texture {
 *       WGPUTexture     texture;
 *       WGPUTextureView view;   // default 2D view over all mips/layers
 *   };
 *
 * -------------------------------------------------------------------------
 * INITIALIZATION (Emscripten-specific)
 * -------------------------------------------------------------------------
 *
 * Standard WebGPU init is async (requestAdapter → requestDevice). In
 * Emscripten, the browser already owns the device; we obtain it
 * synchronously via the Emscripten extension:
 *
 *   #include <emscripten/emscripten.h>
 *   WGPUDevice device = emscripten_webgpu_get_device();
 *   WGPUQueue  queue  = wgpuDeviceGetQueue(device);
 *
 * Surface / swapchain setup (emdawnwebgpu / modern Emscripten):
 *
 *   WGPUSurface surface = wgpuInstanceCreateSurface(instance,
 *       &(WGPUSurfaceDescriptor){
 *           .nextInChain = (WGPUChainedStruct *)&(
 *               WGPUSurfaceSourceHTMLCanvasIdFromDOM){
 *               .chain = { .sType =
 *                   WGPUSType_SurfaceSourceHTMLCanvasIdFromDOM },
 *               .selector = "canvas",
 *           },
 *       });
 *
 *   wgpuSurfaceConfigure(surface, &(WGPUSurfaceConfiguration){
 *       .device = device,
 *       .format = WGPUTextureFormat_BGRA8Unorm,  // browser swapchain format
 *       .usage  = WGPUTextureUsage_RenderAttachment,
 *       .width  = canvas_width,
 *       .height = canvas_height,
 *   });
 *
 * Each frame, obtain the current swapchain texture:
 *
 *   WGPUSurfaceTexture st;
 *   wgpuSurfaceGetCurrentTexture(surface, &st);
 *   WGPUTextureView frame_view = wgpuTextureCreateView(st.texture, NULL);
 *   // ... render into frame_view ...
 *   wgpuSurfacePresent(surface);
 *   wgpuTextureViewRelease(frame_view);
 *
 * -------------------------------------------------------------------------
 * COMMAND BUFFERS: cmd_buf_begin / cmd_buf_submit
 * -------------------------------------------------------------------------
 *
 * cmd_buf_begin():
 *   Allocates a webgpu_cmd_buf, then:
 *     WGPUCommandEncoder enc =
 *         wgpuDeviceCreateCommandEncoder(device, NULL);
 *     return &(webgpu_cmd_buf){ .encoder = enc, .render_pass = NULL };
 *
 * cmd_buf_submit(cmd):
 *   Assert no render pass is still open.
 *     WGPUCommandBuffer cb = wgpuCommandEncoderFinish(enc, NULL);
 *     wgpuQueueSubmit(queue, 1, &cb);
 *     wgpuCommandBufferRelease(cb);
 *     wgpuCommandEncoderRelease(enc);
 *     free(cmd);
 *
 * -------------------------------------------------------------------------
 * PIPELINES: pipeline_create / pipeline_destroy / cmd_set_pipeline
 * -------------------------------------------------------------------------
 *
 * pipeline_create(desc):
 *   WGPURenderPipelineDescriptor wdesc = {
 *       .primitive = {
 *           .topology         = <topology_map[desc->topology]>,
 *           .stripIndexFormat = <strip topologies only>
 *               GPU_INDEX_FORMAT_UINT16 → WGPUIndexFormat_Uint16
 *               GPU_INDEX_FORMAT_UINT32 → WGPUIndexFormat_Uint32
 *               list/point topologies  → WGPUIndexFormat_Undefined
 *       },
 *       .multisample = { .count = desc->sample_count, .mask = ~0u },
 *       .vertex   = { .module = <shader>, .entryPoint = "vs_main" },
 *       .fragment = &(WGPUFragmentState){
 *           .module      = <shader>,
 *           .entryPoint  = "fs_main",
 *           .targetCount = desc->color_format_count,
 *           .targets     = <format_targets>,
 *       },
 *       // depthStencil set if desc->depth_format != GPU_FORMAT_UNKNOWN
 *   };
 *   return (gpu_pipeline_t)wgpuDeviceCreateRenderPipeline(device, &wdesc);
 *
 * Topology map:
 *   GPU_TOPOLOGY_TRIANGLE_LIST  → WGPUPrimitiveTopology_TriangleList
 *   GPU_TOPOLOGY_TRIANGLE_STRIP → WGPUPrimitiveTopology_TriangleStrip
 *   GPU_TOPOLOGY_LINE_LIST      → WGPUPrimitiveTopology_LineList
 *   GPU_TOPOLOGY_POINT_LIST     → WGPUPrimitiveTopology_PointList
 *
 * pipeline_destroy(pipeline):
 *   wgpuRenderPipelineRelease((WGPURenderPipeline)pipeline);
 *
 * cmd_set_pipeline(cmd, pipeline):
 *   Requires an active render pass (render_pass != NULL):
 *   wgpuRenderPassEncoderSetPipeline(cmd->render_pass,
 *                                    (WGPURenderPipeline)pipeline);
 *
 * -------------------------------------------------------------------------
 * RENDER PASSES: cmd_begin_render_pass / cmd_end_render_pass
 * -------------------------------------------------------------------------
 *
 * cmd_begin_render_pass(cmd, desc):
 *   Build WGPURenderPassColorAttachment[desc->color_count]:
 *     Each gpu_color_attachment maps as:
 *       .view     = ((struct webgpu_texture *)att->texture)->view,
 *       .loadOp   = <load_op_map[att->load_op]>,
 *       .storeOp  = <store_op_map[att->store_op]>,
 *       .clearValue = { att->clear[0..3] },
 *       .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED,
 *
 *   Load op map:
 *     GPU_LOAD_OP_LOAD      → WGPULoadOp_Load
 *     GPU_LOAD_OP_CLEAR     → WGPULoadOp_Clear
 *     GPU_LOAD_OP_DONT_CARE → WGPULoadOp_Undefined  (WebGPU has no don't-care
 *                                                     for load; Undefined is
 *                                                     the closest equivalent)
 *   Store op map:
 *     GPU_STORE_OP_STORE     → WGPUStoreOp_Store
 *     GPU_STORE_OP_DONT_CARE → WGPUStoreOp_Discard
 *
 *   Depth attachment (if desc->depth != NULL):
 *     WGPURenderPassDepthStencilAttachment datt = {
 *         .view          = <depth texture view>,
 *         .depthLoadOp   = <load_op_map[desc->depth_load_op]>,
 *         .depthStoreOp  = <store_op_map[desc->depth_store_op]>,
 *         .depthClearValue = desc->clear_depth,
 *     };
 *
 *   cmd->render_pass = wgpuCommandEncoderBeginRenderPass(
 *       cmd->encoder, &rp_desc);
 *
 * cmd_end_render_pass(cmd):
 *   wgpuRenderPassEncoderEnd(cmd->render_pass);
 *   wgpuRenderPassEncoderRelease(cmd->render_pass);
 *   cmd->render_pass = NULL;
 *
 * -------------------------------------------------------------------------
 * BARRIERS: cmd_barrier  [MISMATCH A — NO-OP]
 * -------------------------------------------------------------------------
 *
 * WebGPU synchronizes resources automatically. The render pass
 * begin/end transitions implicitly cover every hazard our barrier API
 * encodes. Our cmd_barrier call becomes a documented no-op:
 *
 *   cmd_barrier(cmd, barriers, count):
 *     (void)cmd; (void)barriers; (void)count;
 *     // WebGPU implicit sync — no explicit barrier needed.
 *
 * The frame graph's barrier declarations remain valuable as design
 * documentation of resource hazards, but generate no GPU commands.
 *
 * -------------------------------------------------------------------------
 * DRAW / DISPATCH  [MISMATCH B — ROOT DATA POINTER → BIND GROUP]
 * -------------------------------------------------------------------------
 *
 * Aaltonen's model: the caller allocates root-data via gpu_malloc, gets a
 * GPU-side pointer via gpu_host_to_device_ptr, writes CPU data through the
 * CPU pointer, and passes the GPU pointer to cmd_draw_indexed as data_gpu.
 * The shader receives it as `const RootData*` — effectively bindless.
 *
 * WebGPU has no GPU virtual addresses or bindless buffer access. Data
 * must be in a WGPUBuffer bound at a known binding slot.
 *
 * Backend strategy — per-draw uniform ring buffer:
 *   - At init: create a large WGPUBuffer (e.g. 4 MB) with
 *       usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst
 *     This is the "root data ring buffer".
 *   - gpu_malloc(size): allocate a CPU-side slab (plain malloc) and record
 *     it in a cpu_ptr → WGPUBuffer entry table (see MISMATCH C below).
 *     Return the CPU-side pointer.
 *   - gpu_host_to_device_ptr(host_ptr): return host_ptr unchanged.
 *     The CPU pointer IS the key into the entry table.
 *   - cmd_draw_indexed(cmd, args, data_gpu):
 *     1. Look up the entry for data_gpu in the table → get the WGPUBuffer
 *        and its byte offset within the ring buffer.
 *     2. wgpuRenderPassEncoderSetBindGroup(rpe, 0, bind_group,
 *                                          1, &byte_offset)
 *        where bind_group wraps the ring buffer at binding 0.
 *     3. wgpuRenderPassEncoderDrawIndexed(rpe, index_count,
 *            instance_count, first_index, vertex_offset, first_instance)
 *
 *   WGSL shader side: binding 0, group 0, uniform struct RootData { ... }
 *   This is the cleanest WebGPU analog to Aaltonen's root pointer.
 *
 * cmd_dispatch(cmd, x, y, z, data_gpu):
 *   Same data_gpu strategy; uses a WGPUComputePassEncoder:
 *     WGPUComputePassEncoder cpe =
 *         wgpuCommandEncoderBeginComputePass(cmd->encoder, NULL);
 *     wgpuComputePassEncoderSetPipeline(cpe, compute_pipeline);
 *     wgpuComputePassEncoderSetBindGroup(cpe, 0, bind_group, 1, &offset);
 *     wgpuComputePassEncoderDispatchWorkgroups(cpe, x, y, z);
 *     wgpuComputePassEncoderEnd(cpe);
 *     wgpuComputePassEncoderRelease(cpe);
 *
 * -------------------------------------------------------------------------
 * MEMORY  [MISMATCH C — PERSISTENT MAPPING → STAGING COPY]
 * -------------------------------------------------------------------------
 *
 * Aaltonen: gpu_malloc returns a pointer to CPU-mapped GPU memory that
 * stays mapped. Writes by the CPU are immediately visible to the GPU.
 * WebGPU has no persistently-mapped GPU memory.
 *
 * Backend strategy — deferred wgpuQueueWriteBuffer:
 *
 *   struct gpu_alloc {
 *       void       *cpu;    // CPU-side staging (malloc'd)
 *       WGPUBuffer  buf;    // WGPUBuffer (Uniform | CopyDst)
 *       size_t      size;
 *   };
 *
 *   gpu_malloc(size):
 *     Allocate a struct gpu_alloc.
 *     cpu = malloc(size);
 *     buf = wgpuDeviceCreateBuffer(device, &(WGPUBufferDescriptor){
 *         .size  = size,
 *         .usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst,
 *     });
 *     Register cpu → alloc in a lookup table.
 *     return cpu;
 *
 *   gpu_free(ptr):
 *     Look up alloc for ptr; wgpuBufferRelease(alloc->buf);
 *     free(alloc->cpu); free(alloc);
 *
 *   gpu_host_to_device_ptr(host_ptr):
 *     return host_ptr;  // identity; resolved to WGPUBuffer at draw time
 *
 *   Before each draw (in cmd_draw_indexed / cmd_dispatch):
 *     Flush dirty CPU allocations: for each alloc whose CPU data was
 *     modified since last upload:
 *       wgpuQueueWriteBuffer(queue, alloc->buf, 0, alloc->cpu, alloc->size);
 *     (Simple v1: flush all allocations before every draw.)
 *
 * -------------------------------------------------------------------------
 * TEXTURES: texture_create / texture_destroy
 * -------------------------------------------------------------------------
 *
 * Format map:
 *   GPU_FORMAT_RGBA8_UNORM    → WGPUTextureFormat_RGBA8Unorm
 *   GPU_FORMAT_BGRA8_UNORM    → WGPUTextureFormat_BGRA8Unorm
 *   GPU_FORMAT_DEPTH32_FLOAT  → WGPUTextureFormat_Depth32Float
 *   GPU_FORMAT_UNKNOWN        → WGPUTextureFormat_Undefined
 *
 * texture_create(desc):
 *   WGPUTextureUsageFlags usage =
 *       WGPUTextureUsage_RenderAttachment |
 *       WGPUTextureUsage_TextureBinding;
 *   if (is_depth(desc->format))
 *       usage |= WGPUTextureUsage_RenderAttachment;  // already set; note
 *                                                     // depth can't be
 *                                                     // TextureBinding
 *                                                     // without extra feature
 *   WGPUTexture tex = wgpuDeviceCreateTexture(device, &(WGPUTextureDescriptor){
 *       .usage         = usage,
 *       .dimension     = WGPUTextureDimension_2D,
 *       .size          = { desc->width, desc->height, 1 },
 *       .format        = <format_map[desc->format]>,
 *       .mipLevelCount = desc->mip_levels ? desc->mip_levels : full_chain,
 *       .sampleCount   = desc->sample_count,
 *   });
 *   WGPUTextureView view = wgpuTextureCreateView(tex, NULL);  // default view
 *   return alloc_webgpu_texture(tex, view);
 *
 * texture_destroy(texture):
 *   wgpuTextureViewRelease(wt->view);
 *   wgpuTextureDestroy(wt->texture);
 *   wgpuTextureRelease(wt->texture);
 *   free(wt);
 *
 * -------------------------------------------------------------------------
 * DESIGN DECISIONS (resolved before implementation)
 * -------------------------------------------------------------------------
 *
 * 1. Root data size: gpu_malloc(size) records the size alongside the
 *    CPU allocation. At draw time, cmd_draw_indexed looks up the size from
 *    that record and passes it to wgpuQueueWriteBuffer. No changes to
 *    gpu_pipeline_desc or the caller ABI are required.
 *
 * 2. Depth texture sampling: depth textures are created with
 *    WGPUTextureUsage_RenderAttachment only. Sampling a depth texture is
 *    a known limitation of this backend and is not supported.
 *
 * 3. Emscripten header path: use <webgpu/webgpu.h> from the emsdk
 *    WebGPU port (USE_WEBGPU=1 linker flag). Do not use the emdawnwebgpu
 *    port headers.
 *
 * 4. Strip index format: gpu_pipeline_desc now carries a strip_index_format
 *    field (GPU_INDEX_FORMAT_UINT16 / UINT32). The backend maps this to
 *    WGPUPrimitiveState.stripIndexFormat for strip topologies and ignores
 *    it for list/point topologies.
 *
 * =========================================================================
 * IMPLEMENTATION
 * =========================================================================
 */

#include "renderer.h"
#include "subsystem.h"
#include "subsystem_manager.h"
#include "log.h"

#include <webgpu/webgpu.h>
#include <emscripten/html5.h>

#include <stdlib.h>
#include <stdint.h>
#include <string.h>

/* ---- Internal handle types ---- */

struct webgpu_cmd_buf {
	WGPUCommandEncoder    encoder;
	WGPURenderPassEncoder render_pass;
};

struct webgpu_texture {
	WGPUTexture     texture;
	WGPUTextureView view;
};

#define MAX_GPU_ALLOCS 256

struct gpu_alloc {
	void       *cpu;
	WGPUBuffer  buf;
	size_t      size;
};

/* ---- Global device state ---- */

static WGPUDevice  g_device;
static WGPUQueue   g_queue;
static WGPUSurface g_surface;

static WGPUShaderModule   g_shader;
static WGPURenderPipeline g_triangle_pipeline;

static struct gpu_alloc g_allocs[MAX_GPU_ALLOCS];
static uint32_t         g_alloc_count;

/*
 * Procedural colored-triangle shader — no vertex buffer or uniforms.
 * vs_main generates clip-space positions from vertex_index;
 * fs_main returns a solid red.
 */
static const char k_triangle_wgsl[] =
	"@vertex\n"
	"fn vs_main(@builtin(vertex_index) vi: u32)"
	" -> @builtin(position) vec4f {\n"
	"    var pos = array<vec2f, 3>(\n"
	"        vec2f( 0.0,  0.5),\n"
	"        vec2f(-0.5, -0.5),\n"
	"        vec2f( 0.5, -0.5),\n"
	"    );\n"
	"    return vec4f(pos[vi], 0.0, 1.0);\n"
	"}\n"
	"@fragment\n"
	"fn fs_main() -> @location(0) vec4f {\n"
	"    return vec4f(1.0, 0.2, 0.1, 1.0);\n"
	"}\n";

/* ---- Format / topology conversion helpers ---- */

static WGPUTextureFormat format_to_wgpu(gpu_format fmt)
{
	switch (fmt) {
	case GPU_FORMAT_RGBA8_UNORM:   return WGPUTextureFormat_RGBA8Unorm;
	case GPU_FORMAT_BGRA8_UNORM:   return WGPUTextureFormat_BGRA8Unorm;
	case GPU_FORMAT_DEPTH32_FLOAT: return WGPUTextureFormat_Depth32Float;
	default:                        return WGPUTextureFormat_Undefined;
	}
}

static WGPUPrimitiveTopology topology_to_wgpu(gpu_topology t)
{
	switch (t) {
	case GPU_TOPOLOGY_TRIANGLE_LIST:  return WGPUPrimitiveTopology_TriangleList;
	case GPU_TOPOLOGY_TRIANGLE_STRIP: return WGPUPrimitiveTopology_TriangleStrip;
	case GPU_TOPOLOGY_LINE_LIST:      return WGPUPrimitiveTopology_LineList;
	case GPU_TOPOLOGY_POINT_LIST:     return WGPUPrimitiveTopology_PointList;
	default:                           return WGPUPrimitiveTopology_TriangleList;
	}
}

static WGPUIndexFormat strip_index_format_to_wgpu(gpu_index_format f,
						   gpu_topology t)
{
	if (t != GPU_TOPOLOGY_TRIANGLE_STRIP)
		return WGPUIndexFormat_Undefined;
	return f == GPU_INDEX_FORMAT_UINT32
		? WGPUIndexFormat_Uint32
		: WGPUIndexFormat_Uint16;
}

static WGPULoadOp load_op_to_wgpu(gpu_load_op op)
{
	switch (op) {
	case GPU_LOAD_OP_LOAD:      return WGPULoadOp_Load;
	case GPU_LOAD_OP_CLEAR:     return WGPULoadOp_Clear;
	/* WebGPU has no load don't-care; Undefined is the closest. */
	case GPU_LOAD_OP_DONT_CARE: return WGPULoadOp_Undefined;
	default:                    return WGPULoadOp_Load;
	}
}

static WGPUStoreOp store_op_to_wgpu(gpu_store_op op)
{
	switch (op) {
	case GPU_STORE_OP_STORE:     return WGPUStoreOp_Store;
	case GPU_STORE_OP_DONT_CARE: return WGPUStoreOp_Discard;
	default:                     return WGPUStoreOp_Store;
	}
}

/* ---- gpu_alloc lookup table ---- */

static struct gpu_alloc *find_alloc(void *cpu)
{
	uint32_t i;

	for (i = 0; i < g_alloc_count; i++) {
		if (g_allocs[i].cpu == cpu)
			return &g_allocs[i];
	}
	return NULL;
}

static void remove_alloc(void *cpu)
{
	uint32_t i;

	for (i = 0; i < g_alloc_count; i++) {
		if (g_allocs[i].cpu == cpu) {
			g_allocs[i] = g_allocs[--g_alloc_count];
			return;
		}
	}
}

/* ---- gpu_api implementations ---- */

static gpu_cmd_buf_t webgpu_cmd_buf_begin(void)
{
	struct webgpu_cmd_buf *cmd = malloc(sizeof(*cmd));

	cmd->encoder    = wgpuDeviceCreateCommandEncoder(g_device, NULL);
	cmd->render_pass = NULL;
	return (gpu_cmd_buf_t)cmd;
}

static void webgpu_cmd_buf_submit(gpu_cmd_buf_t handle)
{
	struct webgpu_cmd_buf *cmd = (struct webgpu_cmd_buf *)handle;
	WGPUCommandBuffer cb;

	cb = wgpuCommandEncoderFinish(cmd->encoder, NULL);
	wgpuQueueSubmit(g_queue, 1, &cb);
	wgpuCommandBufferRelease(cb);
	wgpuCommandEncoderRelease(cmd->encoder);
	free(cmd);
}

static gpu_pipeline_t
webgpu_pipeline_create(const struct gpu_pipeline_desc *desc)
{
	WGPUColorTargetState targets[GPU_MAX_COLOR_ATTACHMENTS];
	WGPUFragmentState    frag;
	WGPURenderPipelineDescriptor wdesc;
	WGPUDepthStencilState ds_state;
	uint32_t i;

	for (i = 0; i < desc->color_format_count; i++) {
		targets[i] = (WGPUColorTargetState){
			.format    = format_to_wgpu(desc->color_formats[i]),
			.writeMask = WGPUColorWriteMask_All,
		};
	}

	frag = (WGPUFragmentState){
		.module      = g_shader,
		.entryPoint  = { .data = "fs_main", .length = WGPU_STRLEN },
		.targetCount = desc->color_format_count,
		.targets     = targets,
	};

	wdesc = (WGPURenderPipelineDescriptor){
		.vertex = {
			.module     = g_shader,
			.entryPoint = { .data = "vs_main", .length = WGPU_STRLEN },
		},
		.primitive = {
			.topology = topology_to_wgpu(desc->topology),
			.stripIndexFormat = strip_index_format_to_wgpu(
				desc->strip_index_format, desc->topology),
		},
		.multisample = {
			.count = desc->sample_count ? desc->sample_count : 1,
			.mask  = ~0u,
		},
		.fragment = &frag,
	};

	if (desc->depth_format != GPU_FORMAT_UNKNOWN) {
		ds_state = (WGPUDepthStencilState){
			.format            = format_to_wgpu(desc->depth_format),
			.depthWriteEnabled = WGPUOptionalBool_True,
			.depthCompare      = WGPUCompareFunction_Less,
		};
		wdesc.depthStencil = &ds_state;
	}

	return (gpu_pipeline_t)wgpuDeviceCreateRenderPipeline(g_device, &wdesc);
}

static void webgpu_pipeline_destroy(gpu_pipeline_t pipeline)
{
	wgpuRenderPipelineRelease((WGPURenderPipeline)pipeline);
}

static void webgpu_cmd_set_pipeline(gpu_cmd_buf_t handle,
				    gpu_pipeline_t pipeline)
{
	struct webgpu_cmd_buf *cmd = (struct webgpu_cmd_buf *)handle;

	wgpuRenderPassEncoderSetPipeline(cmd->render_pass,
					 (WGPURenderPipeline)pipeline);
}

static void webgpu_cmd_begin_render_pass(gpu_cmd_buf_t handle,
					 const struct gpu_render_pass_desc *desc)
{
	struct webgpu_cmd_buf *cmd = (struct webgpu_cmd_buf *)handle;
	WGPURenderPassColorAttachment color_atts[GPU_MAX_COLOR_ATTACHMENTS];
	WGPURenderPassDescriptor      rp_desc;
	WGPURenderPassDepthStencilAttachment ds_att;
	uint32_t i;

	for (i = 0; i < desc->color_count; i++) {
		const struct gpu_color_attachment *a = &desc->color[i];
		struct webgpu_texture *wt = (struct webgpu_texture *)a->texture;

		color_atts[i] = (WGPURenderPassColorAttachment){
			.view       = wt->view,
			.loadOp     = load_op_to_wgpu(a->load_op),
			.storeOp    = store_op_to_wgpu(a->store_op),
			.clearValue = {
				a->clear[0], a->clear[1],
				a->clear[2], a->clear[3],
			},
			.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED,
		};
	}

	rp_desc = (WGPURenderPassDescriptor){
		.colorAttachmentCount = desc->color_count,
		.colorAttachments     = color_atts,
	};

	if (desc->depth != NULL) {
		struct webgpu_texture *wt = (struct webgpu_texture *)desc->depth;

		ds_att = (WGPURenderPassDepthStencilAttachment){
			.view             = wt->view,
			.depthLoadOp      = load_op_to_wgpu(desc->depth_load_op),
			.depthStoreOp     = store_op_to_wgpu(desc->depth_store_op),
			.depthClearValue  = desc->clear_depth,
		};
		rp_desc.depthStencilAttachment = &ds_att;
	}

	cmd->render_pass = wgpuCommandEncoderBeginRenderPass(
		cmd->encoder, &rp_desc);
}

static void webgpu_cmd_end_render_pass(gpu_cmd_buf_t handle)
{
	struct webgpu_cmd_buf *cmd = (struct webgpu_cmd_buf *)handle;

	wgpuRenderPassEncoderEnd(cmd->render_pass);
	wgpuRenderPassEncoderRelease(cmd->render_pass);
	cmd->render_pass = NULL;
}

/* [MISMATCH A] WebGPU implicit sync — barriers are a documented no-op. */
static void webgpu_cmd_barrier(gpu_cmd_buf_t cmd,
				const struct gpu_barrier *barriers,
				uint32_t count)
{
	(void)cmd; (void)barriers; (void)count;
}

/*
 * [MISMATCH B] data_gpu is a CPU pointer into g_allocs (identity mapping
 * from gpu_host_to_device_ptr). Flush the staging copy before drawing.
 * Bind group wiring is deferred until shaders declare uniform bindings.
 */
static void webgpu_cmd_draw_indexed(gpu_cmd_buf_t handle,
				    const struct gpu_draw_indexed_args *args,
				    void *data_gpu)
{
	struct webgpu_cmd_buf *cmd = (struct webgpu_cmd_buf *)handle;
	struct gpu_alloc *alloc;

	if (data_gpu != NULL) {
		alloc = find_alloc(data_gpu);
		if (alloc != NULL) {
			wgpuQueueWriteBuffer(g_queue, alloc->buf, 0,
					     alloc->cpu, alloc->size);
		}
	}

	wgpuRenderPassEncoderDraw(cmd->render_pass,
				  args->index_count,
				  args->instance_count ? args->instance_count : 1,
				  args->first_index,
				  args->first_instance);
}

static void webgpu_cmd_dispatch(gpu_cmd_buf_t handle,
				uint32_t x, uint32_t y, uint32_t z,
				void *data_gpu)
{
	struct webgpu_cmd_buf *cmd = (struct webgpu_cmd_buf *)handle;
	struct gpu_alloc *alloc;
	WGPUComputePassEncoder cpe;

	if (data_gpu != NULL) {
		alloc = find_alloc(data_gpu);
		if (alloc != NULL) {
			wgpuQueueWriteBuffer(g_queue, alloc->buf, 0,
					     alloc->cpu, alloc->size);
		}
	}

	cpe = wgpuCommandEncoderBeginComputePass(cmd->encoder, NULL);
	wgpuComputePassEncoderDispatchWorkgroups(cpe, x, y, z);
	wgpuComputePassEncoderEnd(cpe);
	wgpuComputePassEncoderRelease(cpe);
}

/* [MISMATCH C] Returns a CPU pointer; a WGPUBuffer is created alongside. */
static void *webgpu_gpu_malloc(size_t size)
{
	void       *cpu;
	WGPUBuffer  buf;

	if (g_alloc_count >= MAX_GPU_ALLOCS)
		return NULL;

	cpu = malloc(size);
	if (cpu == NULL)
		return NULL;

	buf = wgpuDeviceCreateBuffer(g_device, &(WGPUBufferDescriptor){
		.size  = (size + 3) & ~(size_t)3,
		.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst,
	});

	g_allocs[g_alloc_count++] = (struct gpu_alloc){
		.cpu  = cpu,
		.buf  = buf,
		.size = size,
	};

	return cpu;
}

static void webgpu_gpu_free(void *ptr)
{
	struct gpu_alloc *alloc = find_alloc(ptr);

	if (alloc == NULL)
		return;
	wgpuBufferRelease(alloc->buf);
	free(alloc->cpu);
	remove_alloc(ptr);
}

/* Identity: the CPU pointer is the key used at draw time. */
static void *webgpu_gpu_host_to_device_ptr(void *host_ptr)
{
	return host_ptr;
}

static gpu_texture_t webgpu_texture_create(const struct gpu_texture_desc *desc)
{
	struct webgpu_texture *wt;
	int                    is_depth;
	WGPUTextureUsage       usage;

	wt       = malloc(sizeof(*wt));
	is_depth = (desc->format == GPU_FORMAT_DEPTH32_FLOAT);
	usage    = WGPUTextureUsage_RenderAttachment;
	if (!is_depth)
		usage |= WGPUTextureUsage_TextureBinding;

	wt->texture = wgpuDeviceCreateTexture(g_device, &(WGPUTextureDescriptor){
		.usage         = usage,
		.dimension     = WGPUTextureDimension_2D,
		.size          = { desc->width, desc->height, 1 },
		.format        = format_to_wgpu(desc->format),
		.mipLevelCount = desc->mip_levels ? desc->mip_levels : 1,
		.sampleCount   = desc->sample_count ? desc->sample_count : 1,
	});
	wt->view = wgpuTextureCreateView(wt->texture, NULL);
	return (gpu_texture_t)wt;
}

static void webgpu_texture_destroy(gpu_texture_t texture)
{
	struct webgpu_texture *wt = (struct webgpu_texture *)texture;

	wgpuTextureViewRelease(wt->view);
	wgpuTextureDestroy(wt->texture);
	wgpuTextureRelease(wt->texture);
	free(wt);
}

/* ---- gpu_api vtable ---- */

static const struct gpu_api webgpu_api = {
	.cmd_buf_begin          = webgpu_cmd_buf_begin,
	.cmd_buf_submit         = webgpu_cmd_buf_submit,
	.pipeline_create        = webgpu_pipeline_create,
	.pipeline_destroy       = webgpu_pipeline_destroy,
	.cmd_set_pipeline       = webgpu_cmd_set_pipeline,
	.cmd_begin_render_pass  = webgpu_cmd_begin_render_pass,
	.cmd_end_render_pass    = webgpu_cmd_end_render_pass,
	.cmd_barrier            = webgpu_cmd_barrier,
	.cmd_draw_indexed       = webgpu_cmd_draw_indexed,
	.cmd_dispatch           = webgpu_cmd_dispatch,
	.gpu_malloc             = webgpu_gpu_malloc,
	.gpu_free               = webgpu_gpu_free,
	.gpu_host_to_device_ptr = webgpu_gpu_host_to_device_ptr,
	.texture_create         = webgpu_texture_create,
	.texture_destroy        = webgpu_texture_destroy,
};

/* ---- Plugin lifecycle ---- */

static void renderer_webgpu_init(void)
{
	WGPUInstance instance;
	int w, h;
	WGPUShaderSourceWGSL wgsl_desc;
	WGPUColorTargetState ct;
	WGPUFragmentState    frag;

	LOG_INFO("renderer_webgpu: init");

	g_device = emscripten_webgpu_get_device();
	g_queue  = wgpuDeviceGetQueue(g_device);

	instance = wgpuCreateInstance(NULL);
	g_surface = wgpuInstanceCreateSurface(instance,
		&(WGPUSurfaceDescriptor){
			.nextInChain = (WGPUChainedStruct *)
				&(WGPUEmscriptenSurfaceSourceCanvasHTMLSelector){
					.chain = { .sType =
					  WGPUSType_EmscriptenSurfaceSourceCanvasHTMLSelector },
					.selector = { .data = "#canvas",
					              .length = WGPU_STRLEN },
				},
		});
	wgpuInstanceRelease(instance);

	emscripten_get_canvas_element_size("#canvas", &w, &h);

	wgpuSurfaceConfigure(g_surface, &(WGPUSurfaceConfiguration){
		.device = g_device,
		.format = WGPUTextureFormat_BGRA8Unorm,
		.usage  = WGPUTextureUsage_RenderAttachment,
		.width  = (uint32_t)w,
		.height = (uint32_t)h,
	});

	wgsl_desc = (WGPUShaderSourceWGSL){
		.chain = { .sType = WGPUSType_ShaderSourceWGSL },
		.code  = { .data = k_triangle_wgsl, .length = WGPU_STRLEN },
	};
	g_shader = wgpuDeviceCreateShaderModule(g_device,
		&(WGPUShaderModuleDescriptor){
			.nextInChain = (WGPUChainedStruct *)&wgsl_desc,
		});

	ct = (WGPUColorTargetState){
		.format    = WGPUTextureFormat_BGRA8Unorm,
		.writeMask = WGPUColorWriteMask_All,
	};
	frag = (WGPUFragmentState){
		.module      = g_shader,
		.entryPoint  = { .data = "fs_main", .length = WGPU_STRLEN },
		.targetCount = 1,
		.targets     = &ct,
	};
	g_triangle_pipeline = wgpuDeviceCreateRenderPipeline(g_device,
		&(WGPURenderPipelineDescriptor){
			.vertex = {
				.module     = g_shader,
				.entryPoint = { .data = "vs_main", .length = WGPU_STRLEN },
			},
			.primitive = {
				.topology = WGPUPrimitiveTopology_TriangleList,
			},
			.multisample = {
				.count = 1,
				.mask  = ~0u,
			},
			.fragment = &frag,
		});

	LOG_INFO("renderer_webgpu: ready");
}

/* Draws the bootstrap triangle into the swapchain each frame. */
static void renderer_webgpu_tick(void)
{
	WGPUSurfaceTexture st;
	WGPUTextureView    frame_view;
	WGPUCommandEncoder enc;
	WGPURenderPassColorAttachment ca;
	WGPURenderPassEncoder rpe;
	WGPUCommandBuffer cb;

	wgpuSurfaceGetCurrentTexture(g_surface, &st);
	if (!st.texture)
		return;

	frame_view = wgpuTextureCreateView(st.texture, NULL);

	enc = wgpuDeviceCreateCommandEncoder(g_device, NULL);

	ca = (WGPURenderPassColorAttachment){
		.view       = frame_view,
		.loadOp     = WGPULoadOp_Clear,
		.storeOp    = WGPUStoreOp_Store,
		.clearValue = { 0.05f, 0.05f, 0.05f, 1.0f },
		.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED,
	};
	rpe = wgpuCommandEncoderBeginRenderPass(enc,
		&(WGPURenderPassDescriptor){
			.colorAttachmentCount = 1,
			.colorAttachments     = &ca,
		});

	wgpuRenderPassEncoderSetPipeline(rpe, g_triangle_pipeline);
	wgpuRenderPassEncoderDraw(rpe, 3, 1, 0, 0);
	wgpuRenderPassEncoderEnd(rpe);
	wgpuRenderPassEncoderRelease(rpe);

	cb = wgpuCommandEncoderFinish(enc, NULL);
	wgpuQueueSubmit(g_queue, 1, &cb);
	wgpuCommandBufferRelease(cb);
	wgpuCommandEncoderRelease(enc);

	wgpuTextureViewRelease(frame_view);
	wgpuSurfacePresent(g_surface);
	wgpuTextureRelease(st.texture);
}

static void renderer_webgpu_shutdown(void)
{
	LOG_INFO("renderer_webgpu: shutdown");
	wgpuRenderPipelineRelease(g_triangle_pipeline);
	wgpuShaderModuleRelease(g_shader);
	wgpuSurfaceRelease(g_surface);
	wgpuQueueRelease(g_queue);
}

static const struct subsystem webgpu_subsystem = {
	.name     = "renderer",
	.api      = &webgpu_api,
	.init     = renderer_webgpu_init,
	.tick     = renderer_webgpu_tick,
	.shutdown = renderer_webgpu_shutdown,
};

void plugin_entry(struct subsystem_manager *mgr)
{
	subsystem_manager_register(mgr, &webgpu_subsystem);
}
