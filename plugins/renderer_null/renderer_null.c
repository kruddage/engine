/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "renderer.h"
#include "renderer_null.h"
#include "log_api.h"
#include "subsystem.h"
#include "subsystem_manager.h"

#include <stddef.h>
#include <stdint.h>

#ifndef __EMSCRIPTEN__
#include "log.h"
static const struct log_api native_log = { log_write };
#endif

#define RENDERER_NULL_LOG_MAX 256

static struct gpu_call_record g_log_buf[RENDERER_NULL_LOG_MAX];
static uint32_t               g_log_count;

#ifdef __EMSCRIPTEN__
static const struct log_api *g_log;
#else
static const struct log_api *g_log = &native_log;
#endif

static void log_append(struct gpu_call_record rec)
{
	if (g_log_count < RENDERER_NULL_LOG_MAX)
		g_log_buf[g_log_count++] = rec;
}

static gpu_cmd_buf_t null_cmd_buf_begin(void)
{
	g_log->write(LOG_LEVEL_INFO, "renderer_null: cmd_buf_begin");
	log_append((struct gpu_call_record){
		.type = GPU_CALL_CMD_BUF_BEGIN,
	});
	return NULL;
}

static void null_cmd_buf_submit(gpu_cmd_buf_t cmd)
{
	g_log->write(LOG_LEVEL_INFO, "renderer_null: cmd_buf_submit cmd=%p", (void *)cmd);
	log_append((struct gpu_call_record){
		.type = GPU_CALL_CMD_BUF_SUBMIT,
	});
}

static gpu_pipeline_t
null_pipeline_create(const struct gpu_pipeline_desc *desc)
{
	g_log->write(LOG_LEVEL_INFO, "renderer_null: pipeline_create color_format_count=%u",
		     desc->color_format_count);
	log_append((struct gpu_call_record){
		.type = GPU_CALL_PIPELINE_CREATE,
		.args = {
			.pipeline_create = {
				.color_format_count = desc->color_format_count,
				.has_vert_src = desc->vert.src != NULL,
				.has_frag_src = desc->frag.src != NULL,
			},
		},
	});
	return NULL;
}

static void null_pipeline_destroy(gpu_pipeline_t pipeline)
{
	g_log->write(LOG_LEVEL_INFO, "renderer_null: pipeline_destroy pipeline=%p",
		     (void *)pipeline);
	log_append((struct gpu_call_record){
		.type = GPU_CALL_PIPELINE_DESTROY,
	});
}

static void null_cmd_set_pipeline(gpu_cmd_buf_t cmd, gpu_pipeline_t pipeline)
{
	g_log->write(LOG_LEVEL_INFO, "renderer_null: cmd_set_pipeline cmd=%p pipeline=%p",
		     (void *)cmd, (void *)pipeline);
	log_append((struct gpu_call_record){
		.type = GPU_CALL_CMD_SET_PIPELINE,
	});
}

static void null_cmd_begin_render_pass(gpu_cmd_buf_t cmd,
				       const struct gpu_render_pass_desc *desc)
{
	g_log->write(LOG_LEVEL_INFO, "renderer_null: cmd_begin_render_pass cmd=%p color_count=%u",
		     (void *)cmd, desc->color_count);
	log_append((struct gpu_call_record){
		.type = GPU_CALL_CMD_BEGIN_RENDER_PASS,
		.args = {
			.cmd_begin_render_pass = {
				.color_count = desc->color_count,
			},
		},
	});
}

static void null_cmd_end_render_pass(gpu_cmd_buf_t cmd)
{
	g_log->write(LOG_LEVEL_INFO, "renderer_null: cmd_end_render_pass cmd=%p", (void *)cmd);
	log_append((struct gpu_call_record){
		.type = GPU_CALL_CMD_END_RENDER_PASS,
	});
}

static void null_cmd_barrier(gpu_cmd_buf_t cmd,
			      const struct gpu_barrier *barriers,
			      uint32_t count)
{
	(void)barriers;
	g_log->write(LOG_LEVEL_INFO, "renderer_null: cmd_barrier cmd=%p count=%u",
		     (void *)cmd, count);
	log_append((struct gpu_call_record){
		.type = GPU_CALL_CMD_BARRIER,
		.args = { .cmd_barrier = { .count = count } },
	});
}

static void null_cmd_draw_indexed(gpu_cmd_buf_t cmd,
				  const struct gpu_draw_indexed_args *args)
{
	g_log->write(LOG_LEVEL_INFO, "renderer_null: cmd_draw_indexed cmd=%p index_count=%u",
		     (void *)cmd, args->index_count);
	log_append((struct gpu_call_record){
		.type = GPU_CALL_CMD_DRAW_INDEXED,
		.args = {
			.cmd_draw_indexed = {
				.index_count    = args->index_count,
				.instance_count = args->instance_count,
			},
		},
	});
}

static void null_cmd_dispatch(gpu_cmd_buf_t cmd,
			      uint32_t x, uint32_t y, uint32_t z)
{
	g_log->write(LOG_LEVEL_INFO, "renderer_null: cmd_dispatch cmd=%p x=%u y=%u z=%u",
		     (void *)cmd, x, y, z);
	log_append((struct gpu_call_record){
		.type = GPU_CALL_CMD_DISPATCH,
		.args = { .cmd_dispatch = { .x = x, .y = y, .z = z } },
	});
}

static gpu_buffer_t null_buffer_create(const struct gpu_buffer_desc *desc)
{
	g_log->write(LOG_LEVEL_INFO, "renderer_null: buffer_create size=%zu usage=%u",
		     desc->size, desc->usage);
	log_append((struct gpu_call_record){
		.type = GPU_CALL_BUFFER_CREATE,
		.args = {
			.buffer_create = {
				.size  = desc->size,
				.usage = desc->usage,
			},
		},
	});
	return NULL;
}

static void null_buffer_destroy(gpu_buffer_t buf)
{
	g_log->write(LOG_LEVEL_INFO, "renderer_null: buffer_destroy buf=%p", (void *)buf);
	log_append((struct gpu_call_record){ .type = GPU_CALL_BUFFER_DESTROY });
}

static void null_buffer_update(gpu_buffer_t buf, uint32_t offset,
			       const void *data, uint32_t size)
{
	(void)data;
	g_log->write(LOG_LEVEL_INFO,
		     "renderer_null: buffer_update buf=%p offset=%u size=%u",
		     (void *)buf, offset, size);
	/* No record type needed; the scene renderer test asserts on draws. */
}

static void null_cmd_bind_vertex_buffer(gpu_cmd_buf_t cmd, uint32_t slot,
					gpu_buffer_t buf, uint32_t offset)
{
	g_log->write(LOG_LEVEL_INFO,
		     "renderer_null: cmd_bind_vertex_buffer cmd=%p slot=%u buf=%p offset=%u",
		     (void *)cmd, slot, (void *)buf, offset);
	log_append((struct gpu_call_record){
		.type = GPU_CALL_CMD_BIND_VERTEX_BUFFER,
		.args = {
			.cmd_bind_vertex_buffer = {
				.slot   = slot,
				.offset = offset,
			},
		},
	});
}

static void null_cmd_bind_index_buffer(gpu_cmd_buf_t cmd, gpu_buffer_t buf,
				       uint32_t offset, gpu_index_format fmt)
{
	g_log->write(LOG_LEVEL_INFO,
		     "renderer_null: cmd_bind_index_buffer cmd=%p buf=%p offset=%u fmt=%u",
		     (void *)cmd, (void *)buf, offset, (unsigned)fmt);
	log_append((struct gpu_call_record){
		.type = GPU_CALL_CMD_BIND_INDEX_BUFFER,
		.args = {
			.cmd_bind_index_buffer = {
				.offset = offset,
				.fmt    = (uint32_t)fmt,
			},
		},
	});
}

static void null_cmd_bind_uniform_buffer(gpu_cmd_buf_t cmd, uint32_t slot,
					 gpu_buffer_t buf, uint32_t offset,
					 uint32_t size)
{
	g_log->write(LOG_LEVEL_INFO,
		     "renderer_null: cmd_bind_uniform_buffer cmd=%p slot=%u buf=%p offset=%u size=%u",
		     (void *)cmd, slot, (void *)buf, offset, size);
	log_append((struct gpu_call_record){
		.type = GPU_CALL_CMD_BIND_UNIFORM_BUFFER,
		.args = {
			.cmd_bind_uniform_buffer = {
				.slot   = slot,
				.offset = offset,
				.size   = size,
			},
		},
	});
}

static void *null_gpu_malloc(size_t size)
{
	g_log->write(LOG_LEVEL_INFO, "renderer_null: gpu_malloc size=%zu", size);
	log_append((struct gpu_call_record){
		.type = GPU_CALL_GPU_MALLOC,
		.args = { .gpu_malloc = { .size = size } },
	});
	return NULL;
}

static void null_gpu_free(void *ptr)
{
	g_log->write(LOG_LEVEL_INFO, "renderer_null: gpu_free ptr=%p", ptr);
	log_append((struct gpu_call_record){ .type = GPU_CALL_GPU_FREE });
}

static void *null_gpu_host_to_device_ptr(void *host_ptr)
{
	g_log->write(LOG_LEVEL_INFO, "renderer_null: gpu_host_to_device_ptr ptr=%p", host_ptr);
	log_append((struct gpu_call_record){
		.type = GPU_CALL_GPU_HOST_TO_DEVICE_PTR,
	});
	return host_ptr;
}

static gpu_texture_t
null_texture_create(const struct gpu_texture_desc *desc)
{
	g_log->write(LOG_LEVEL_INFO, "renderer_null: texture_create fmt=%u w=%u h=%u",
		     (unsigned)desc->format, desc->width, desc->height);
	log_append((struct gpu_call_record){
		.type = GPU_CALL_TEXTURE_CREATE,
		.args = {
			.texture_create = {
				.format = (uint32_t)desc->format,
				.width  = desc->width,
				.height = desc->height,
			},
		},
	});
	return NULL;
}

static void null_texture_destroy(gpu_texture_t texture)
{
	g_log->write(LOG_LEVEL_INFO, "renderer_null: texture_destroy texture=%p", (void *)texture);
	log_append((struct gpu_call_record){
		.type = GPU_CALL_TEXTURE_DESTROY,
	});
}

static const struct gpu_api null_api = {
	.caps                   = GPU_CAP_DRAW_INDEXED | GPU_CAP_COMPUTE,
	.cmd_buf_begin          = null_cmd_buf_begin,
	.cmd_buf_submit         = null_cmd_buf_submit,
	.pipeline_create        = null_pipeline_create,
	.pipeline_destroy       = null_pipeline_destroy,
	.cmd_set_pipeline       = null_cmd_set_pipeline,
	.buffer_create          = null_buffer_create,
	.buffer_destroy         = null_buffer_destroy,
	.buffer_update          = null_buffer_update,
	.cmd_bind_vertex_buffer = null_cmd_bind_vertex_buffer,
	.cmd_bind_index_buffer  = null_cmd_bind_index_buffer,
	.cmd_bind_uniform_buffer = null_cmd_bind_uniform_buffer,
	.cmd_begin_render_pass  = null_cmd_begin_render_pass,
	.cmd_end_render_pass    = null_cmd_end_render_pass,
	.cmd_barrier            = null_cmd_barrier,
	.cmd_draw_indexed       = null_cmd_draw_indexed,
	.cmd_dispatch           = null_cmd_dispatch,
	.gpu_malloc             = null_gpu_malloc,
	.gpu_free               = null_gpu_free,
	/* Bindless-only; null renderer does not set GPU_CAP_BINDLESS. */
	.gpu_host_to_device_ptr = NULL,
	.texture_create         = null_texture_create,
	.texture_destroy        = null_texture_destroy,
};

static void renderer_null_init(void)
{
	/*
	 * Referenced so the bindless-only host_to_device_ptr impl is retained
	 * for a future GPU_CAP_BINDLESS path without tripping -Wunused-function.
	 */
	(void)null_gpu_host_to_device_ptr;
	g_log->write(LOG_LEVEL_INFO, "renderer_null: init");
	renderer_null_reset_log();
}

static void renderer_null_shutdown(void)
{
	g_log->write(LOG_LEVEL_INFO, "renderer_null: shutdown");
}

static const struct subsystem desc = {
	.name     = "renderer",
	.api      = &null_api,
	.init     = renderer_null_init,
	.shutdown = renderer_null_shutdown,
};

#ifdef __EMSCRIPTEN__
void plugin_entry(struct subsystem_manager *mgr)
#else
void renderer_null_plugin_entry(struct subsystem_manager *mgr)
#endif
{
#ifdef __EMSCRIPTEN__
	g_log = subsystem_manager_get_api(mgr, "log");
#endif
	subsystem_manager_register(mgr, &desc);
}

const struct gpu_call_record *renderer_null_get_log(uint32_t *out_count)
{
	*out_count = g_log_count;
	return g_log_buf;
}

void renderer_null_reset_log(void)
{
	g_log_count = 0;
}
