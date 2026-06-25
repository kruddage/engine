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
 * IMPLEMENTATION STARTS HERE (not yet written)
 * =========================================================================
 */
