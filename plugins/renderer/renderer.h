/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RENDERER_H
#define RENDERER_H

#include <stddef.h>
#include <stdint.h>

/*
 * Low-level GPU API — shaped by Aaltonen's "No Graphics API" philosophy.
 * Exposes GPU primitives directly; no game-level drawing concepts.
 *
 * Backends register a struct gpu_api * as the "renderer" subsystem api:
 *   subsystem_manager_get_api(mgr, "renderer") → const struct gpu_api *
 */

/* --- Opaque handle types ------------------------------------------------- */

typedef struct gpu_cmd_buf  *gpu_cmd_buf_t;
typedef struct gpu_pipeline *gpu_pipeline_t;
typedef struct gpu_texture  *gpu_texture_t;

/* --- Enumerations -------------------------------------------------------- */

typedef enum {
	GPU_FORMAT_UNKNOWN = 0,
	GPU_FORMAT_RGBA8_UNORM,
	GPU_FORMAT_BGRA8_UNORM,
	GPU_FORMAT_DEPTH32_FLOAT,
} gpu_format;

typedef enum {
	GPU_TOPOLOGY_TRIANGLE_LIST = 0,
	GPU_TOPOLOGY_TRIANGLE_STRIP,
	GPU_TOPOLOGY_LINE_LIST,
	GPU_TOPOLOGY_POINT_LIST,
} gpu_topology;

typedef enum {
	GPU_LOAD_OP_LOAD = 0,
	GPU_LOAD_OP_CLEAR,
	GPU_LOAD_OP_DONT_CARE,
} gpu_load_op;

typedef enum {
	GPU_STORE_OP_STORE = 0,
	GPU_STORE_OP_DONT_CARE,
} gpu_store_op;

/* --- Pipeline state object ----------------------------------------------- */

#define GPU_MAX_COLOR_ATTACHMENTS 8

/*
 * Minimal baked state: render target formats, topology, sample count.
 * Depth-stencil and blend state are set independently via cmd calls.
 */
struct gpu_pipeline_desc {
	gpu_format   color_formats[GPU_MAX_COLOR_ATTACHMENTS];
	uint32_t     color_format_count;
	gpu_format   depth_format;   /* GPU_FORMAT_UNKNOWN if no depth */
	gpu_topology topology;
	uint32_t     sample_count;
};

/* --- Render passes ------------------------------------------------------- */

struct gpu_color_attachment {
	gpu_texture_t texture;
	gpu_load_op   load_op;
	gpu_store_op  store_op;
	float         clear[4];      /* rgba; used when load_op == CLEAR */
};

/* Transient: begin/end only; no persistent Vulkan-style pass objects. */
struct gpu_render_pass_desc {
	struct gpu_color_attachment color[GPU_MAX_COLOR_ATTACHMENTS];
	uint32_t                    color_count;
	gpu_texture_t               depth;
	gpu_load_op                 depth_load_op;
	gpu_store_op                depth_store_op;
	float                       clear_depth;
};

/* --- Barriers ------------------------------------------------------------ */

typedef uint32_t gpu_stage_mask;
#define GPU_STAGE_TOP      (1u << 0)
#define GPU_STAGE_VERTEX   (1u << 1)
#define GPU_STAGE_FRAGMENT (1u << 2)
#define GPU_STAGE_COMPUTE  (1u << 3)
#define GPU_STAGE_TRANSFER (1u << 4)
#define GPU_STAGE_BOTTOM   (1u << 5)

typedef uint32_t gpu_access_mask;
#define GPU_ACCESS_SHADER_READ    (1u << 0)
#define GPU_ACCESS_SHADER_WRITE   (1u << 1)
#define GPU_ACCESS_COLOR_WRITE    (1u << 2)
#define GPU_ACCESS_DEPTH_WRITE    (1u << 3)
#define GPU_ACCESS_TRANSFER_READ  (1u << 4)
#define GPU_ACCESS_TRANSFER_WRITE (1u << 5)

/* Stage-mask pair + sparse hazard flags; no per-resource lists. */
struct gpu_barrier {
	gpu_stage_mask  src_stage;
	gpu_stage_mask  dst_stage;
	gpu_access_mask src_access;
	gpu_access_mask dst_access;
	uint32_t        hazard_flags; /* backend-defined sparse bits */
};

/* --- Texture descriptors ------------------------------------------------- */

struct gpu_texture_desc {
	gpu_format format;
	uint32_t   width;
	uint32_t   height;
	uint32_t   mip_levels;   /* 0 → full mip chain */
	uint32_t   sample_count; /* 1 for no MSAA */
};

/* --- Draw / dispatch ----------------------------------------------------- */

struct gpu_draw_indexed_args {
	uint32_t index_count;
	uint32_t instance_count;
	uint32_t first_index;
	int32_t  vertex_offset;
	uint32_t first_instance;
};

/* --- GPU API vtable ------------------------------------------------------ */

/*
 * Registered with subsystem_manager as the "renderer" api.
 * Retrieve with:
 *   gpu = subsystem_manager_get_api(mgr, "renderer");
 */
struct gpu_api {
	/* Command buffers */
	gpu_cmd_buf_t (*cmd_buf_begin)(void);
	void          (*cmd_buf_submit)(gpu_cmd_buf_t cmd);

	/* Pipelines */
	gpu_pipeline_t (*pipeline_create)(const struct gpu_pipeline_desc *desc);
	void           (*pipeline_destroy)(gpu_pipeline_t pipeline);
	void           (*cmd_set_pipeline)(gpu_cmd_buf_t cmd,
	                                   gpu_pipeline_t pipeline);

	/* Render passes */
	void (*cmd_begin_render_pass)(gpu_cmd_buf_t cmd,
	                              const struct gpu_render_pass_desc *desc);
	void (*cmd_end_render_pass)(gpu_cmd_buf_t cmd);

	/* Barriers */
	void (*cmd_barrier)(gpu_cmd_buf_t cmd,
	                    const struct gpu_barrier *barriers,
	                    uint32_t count);

	/*
	 * data_gpu is a GPU-addressable pointer to the caller-defined root
	 * data struct; the shader receives it as `const RootData *`.
	 */
	void (*cmd_draw_indexed)(gpu_cmd_buf_t cmd,
	                         const struct gpu_draw_indexed_args *args,
	                         void *data_gpu);
	void (*cmd_dispatch)(gpu_cmd_buf_t cmd,
	                     uint32_t x, uint32_t y, uint32_t z,
	                     void *data_gpu);

	/* Memory — returns CPU-mapped GPU memory */
	void *(*gpu_malloc)(size_t size);
	void  (*gpu_free)(void *ptr);
	/* Returns the GPU-side address of a cpu-mapped allocation. */
	void *(*gpu_host_to_device_ptr)(void *host_ptr);

	/* Textures */
	gpu_texture_t (*texture_create)(const struct gpu_texture_desc *desc);
	void          (*texture_destroy)(gpu_texture_t texture);
};

#endif /* RENDERER_H */
