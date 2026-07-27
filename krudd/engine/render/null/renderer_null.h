/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RENDERER_NULL_H
#define RENDERER_NULL_H

#include <stddef.h>
#include <stdint.h>

/*
 * In-memory call log for the null renderer — test support only.
 * Call renderer_null_reset_log() before each test case, then inspect
 * the array returned by renderer_null_get_log() after exercising the API.
 */

enum gpu_call_type {
	GPU_CALL_CMD_BUF_BEGIN,
	GPU_CALL_CMD_BUF_SUBMIT,
	GPU_CALL_FRAME_END,
	GPU_CALL_PIPELINE_CREATE,
	GPU_CALL_PIPELINE_DESTROY,
	GPU_CALL_CMD_SET_PIPELINE,
	GPU_CALL_CMD_BEGIN_RENDER_PASS,
	GPU_CALL_CMD_END_RENDER_PASS,
	GPU_CALL_CMD_BARRIER,
	GPU_CALL_CMD_DRAW_INDEXED,
	GPU_CALL_CMD_DISPATCH,
	GPU_CALL_BUFFER_CREATE,
	GPU_CALL_BUFFER_DESTROY,
	GPU_CALL_CMD_BIND_VERTEX_BUFFER,
	GPU_CALL_CMD_BIND_INDEX_BUFFER,
	GPU_CALL_CMD_BIND_UNIFORM_BUFFER,
	GPU_CALL_GPU_MALLOC,
	GPU_CALL_GPU_FREE,
	GPU_CALL_GPU_HOST_TO_DEVICE_PTR,
	GPU_CALL_TEXTURE_CREATE,
	GPU_CALL_TEXTURE_DESTROY,
	GPU_CALL_CMD_BIND_TEXTURE,
};

struct gpu_call_record {
	enum gpu_call_type type;
	union {
		struct {
			uint32_t color_format_count;
			int      has_vert_src; /* 1 if desc->vert.src != NULL */
			int      has_frag_src; /* 1 if desc->frag.src != NULL */
		} pipeline_create;
		struct {
			uint32_t color_count;
			/* 1 if color[0] carried a resolve target (MSAA resolve). */
			int      color0_resolve;
		} cmd_begin_render_pass;
		struct { uint32_t count;              } cmd_barrier;
		struct {
			uint32_t index_count;
			uint32_t instance_count;
		} cmd_draw_indexed;
		struct { uint32_t x; uint32_t y; uint32_t z; } cmd_dispatch;
		struct {
			size_t   size;
			uint32_t usage;
		} buffer_create;
		struct {
			uint32_t slot;
			uint32_t offset;
		} cmd_bind_vertex_buffer;
		struct {
			uint32_t offset;
			uint32_t fmt; /* gpu_index_format as uint32_t */
		} cmd_bind_index_buffer;
		struct {
			uint32_t slot;
			uint32_t offset;
			uint32_t size;
		} cmd_bind_uniform_buffer;
		struct { size_t size;                         } gpu_malloc;
		struct {
			uint32_t format; /* gpu_format, stored as uint32_t */
			uint32_t width;
			uint32_t height;
			uint32_t mip_levels;
			int      has_initial_data; /* 1 if desc->initial_data != NULL */
			uint32_t generate_mips;
		} texture_create;
		struct { uint32_t unit; } cmd_bind_texture;
	} args;
};

const struct gpu_call_record *renderer_null_get_log(uint32_t *out_count);
void                          renderer_null_reset_log(void);

#endif /* RENDERER_NULL_H */
