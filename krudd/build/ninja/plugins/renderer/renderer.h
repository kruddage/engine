/* SPDX-License-Identifier: GPL-2.0-or-later */
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
typedef struct gpu_buffer   *gpu_buffer_t;

/* --- Capability flags ---------------------------------------------------- */

typedef enum {
	GPU_CAP_DRAW_DIRECT  = 1u << 0,
	GPU_CAP_DRAW_INDEXED = 1u << 1,
	GPU_CAP_COMPUTE      = 1u << 2,
	GPU_CAP_BINDLESS     = 1u << 3, /* reserved; enables pointer/bindless path */
} gpu_cap;

/* --- Enumerations -------------------------------------------------------- */

typedef enum {
	GPU_FORMAT_UNKNOWN = 0,
	GPU_FORMAT_RGBA8_UNORM,
	GPU_FORMAT_BGRA8_UNORM,
	GPU_FORMAT_DEPTH32_FLOAT,
	/* Float-vector formats, used to type vertex attributes. */
	GPU_FORMAT_RG32_FLOAT,
	GPU_FORMAT_RGB32_FLOAT,
	GPU_FORMAT_RGBA32_FLOAT,
} gpu_format;

typedef enum {
	GPU_TOPOLOGY_TRIANGLE_LIST = 0,
	GPU_TOPOLOGY_TRIANGLE_STRIP,
	GPU_TOPOLOGY_LINE_LIST,
	GPU_TOPOLOGY_POINT_LIST,
} gpu_topology;

typedef enum {
	GPU_INDEX_FORMAT_UINT16 = 0,
	GPU_INDEX_FORMAT_UINT32,
} gpu_index_format;

typedef enum {
	GPU_LOAD_OP_LOAD = 0,
	GPU_LOAD_OP_CLEAR,
	GPU_LOAD_OP_DONT_CARE,
} gpu_load_op;

typedef enum {
	GPU_STORE_OP_STORE = 0,
	GPU_STORE_OP_DONT_CARE,
} gpu_store_op;

/* --- Buffers ------------------------------------------------------------- */

typedef enum {
	GPU_BUFFER_USAGE_VERTEX  = 1u << 0,
	GPU_BUFFER_USAGE_INDEX   = 1u << 1,
	GPU_BUFFER_USAGE_UNIFORM = 1u << 2,
	GPU_BUFFER_USAGE_STORAGE = 1u << 3,
} gpu_buffer_usage;

struct gpu_buffer_desc {
	size_t      size;
	uint32_t    usage;        /* bitmask of gpu_buffer_usage */
	const void *initial_data; /* NULL = no upload */
};

/* --- Shaders ------------------------------------------------------------- */

typedef enum {
	GPU_SHADER_STAGE_VERTEX = 0,
	GPU_SHADER_STAGE_FRAGMENT,
} gpu_shader_stage;

typedef enum {
	GPU_SHADER_DIALECT_GLSL_ES_300 = 0,
} gpu_shader_dialect;

struct gpu_shader_source {
	const char        *src;
	gpu_shader_stage   stage;
	gpu_shader_dialect dialect;
};

/* --- Vertex input layout ------------------------------------------------- */

#define GPU_MAX_VERTEX_ATTRS 8

/*
 * One vertex attribute: the shader input `location`, its byte `offset` within
 * the vertex, and its component `format` (a float-vector gpu_format). All
 * attributes source from vertex buffer slot 0 (single interleaved stream).
 */
struct gpu_vertex_attr {
	uint32_t   location;
	uint32_t   offset;
	gpu_format format;
};

struct gpu_vertex_layout {
	struct gpu_vertex_attr attrs[GPU_MAX_VERTEX_ATTRS];
	uint32_t               attr_count; /* 0 = no vertex input declared */
	uint32_t               stride;     /* bytes between consecutive vertices */
};

/* --- Pipeline state object ----------------------------------------------- */

#define GPU_MAX_COLOR_ATTACHMENTS 8

/*
 * Minimal baked state: render target formats, topology, sample count, and the
 * vertex input layout. Depth-stencil and blend state are set independently via
 * cmd calls.
 */
struct gpu_pipeline_desc {
	gpu_format      color_formats[GPU_MAX_COLOR_ATTACHMENTS];
	uint32_t        color_format_count;
	gpu_format      depth_format;        /* GPU_FORMAT_UNKNOWN if no depth */
	gpu_topology    topology;
	gpu_index_format strip_index_format; /* only used for strip topologies */
	uint32_t        sample_count;
	struct gpu_vertex_layout vertex_layout;
	/* Shader sources — renderer owns compile/link at pipeline_create time */
	struct gpu_shader_source vert;
	struct gpu_shader_source frag;
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
	uint32_t caps; /* bitmask of gpu_cap values set by backend during init */

	/* Command buffers */
	gpu_cmd_buf_t (*cmd_buf_begin)(void);
	void          (*cmd_buf_submit)(gpu_cmd_buf_t cmd);

	/* Pipelines */
	gpu_pipeline_t (*pipeline_create)(const struct gpu_pipeline_desc *desc);
	void           (*pipeline_destroy)(gpu_pipeline_t pipeline);
	void           (*cmd_set_pipeline)(gpu_cmd_buf_t cmd,
	                                   gpu_pipeline_t pipeline);

	/* Buffers */
	gpu_buffer_t (*buffer_create)(const struct gpu_buffer_desc *desc);
	void         (*buffer_destroy)(gpu_buffer_t buf);
	/*
	 * Overwrite size bytes at byte offset of a buffer's contents. The write
	 * path for per-frame root data (e.g. a UBO's viewproj/model) that
	 * buffer_create's initial_data cannot supply.
	 */
	void         (*buffer_update)(gpu_buffer_t buf, uint32_t offset,
	                              const void *data, uint32_t size);
	void         (*cmd_bind_vertex_buffer)(gpu_cmd_buf_t cmd, uint32_t slot,
	                                       gpu_buffer_t buf, uint32_t offset);
	void         (*cmd_bind_index_buffer)(gpu_cmd_buf_t cmd, gpu_buffer_t buf,
	                                      uint32_t offset, gpu_index_format fmt);
	void         (*cmd_bind_uniform_buffer)(gpu_cmd_buf_t cmd, uint32_t slot,
	                                        gpu_buffer_t buf, uint32_t offset,
	                                        uint32_t size);

	/* Render passes */
	void (*cmd_begin_render_pass)(gpu_cmd_buf_t cmd,
	                              const struct gpu_render_pass_desc *desc);
	void (*cmd_end_render_pass)(gpu_cmd_buf_t cmd);

	/* Barriers */
	void (*cmd_barrier)(gpu_cmd_buf_t cmd,
	                    const struct gpu_barrier *barriers,
	                    uint32_t count);

	/*
	 * Root data is supplied via a uniform buffer bound with
	 * cmd_bind_uniform_buffer before the draw/dispatch.
	 */
	void (*cmd_draw_indexed)(gpu_cmd_buf_t cmd,
	                         const struct gpu_draw_indexed_args *args);
	void (*cmd_dispatch)(gpu_cmd_buf_t cmd,
	                     uint32_t x, uint32_t y, uint32_t z);

	/* Memory — returns CPU-mapped GPU memory */
	void *(*gpu_malloc)(size_t size);
	void  (*gpu_free)(void *ptr);
	/* Optional — only valid when GPU_CAP_BINDLESS is set. NULL otherwise. */
	void *(*gpu_host_to_device_ptr)(void *host_ptr);

	/* Textures */
	gpu_texture_t (*texture_create)(const struct gpu_texture_desc *desc);
	void          (*texture_destroy)(gpu_texture_t texture);
};

#endif /* RENDERER_H */
