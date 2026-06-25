/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "renderer.h"
#include "renderer_null.h"
#include "log.h"
#include "subsystem.h"
#include "subsystem_manager.h"

#include <stddef.h>
#include <stdint.h>

#define RENDERER_NULL_LOG_MAX 256

static struct gpu_call_record g_log[RENDERER_NULL_LOG_MAX];
static uint32_t               g_log_count;

static void log_append(struct gpu_call_record rec)
{
	if (g_log_count < RENDERER_NULL_LOG_MAX)
		g_log[g_log_count++] = rec;
}

static gpu_cmd_buf_t null_cmd_buf_begin(void)
{
	LOG_INFO("renderer_null: cmd_buf_begin");
	log_append((struct gpu_call_record){
		.type = GPU_CALL_CMD_BUF_BEGIN,
	});
	return NULL;
}

static void null_cmd_buf_submit(gpu_cmd_buf_t cmd)
{
	LOG_INFO("renderer_null: cmd_buf_submit cmd=%p", (void *)cmd);
	log_append((struct gpu_call_record){
		.type = GPU_CALL_CMD_BUF_SUBMIT,
	});
}

static gpu_pipeline_t
null_pipeline_create(const struct gpu_pipeline_desc *desc)
{
	LOG_INFO("renderer_null: pipeline_create color_format_count=%u",
		 desc->color_format_count);
	log_append((struct gpu_call_record){
		.type = GPU_CALL_PIPELINE_CREATE,
		.args = {
			.pipeline_create = {
				.color_format_count = desc->color_format_count,
			},
		},
	});
	return NULL;
}

static void null_pipeline_destroy(gpu_pipeline_t pipeline)
{
	LOG_INFO("renderer_null: pipeline_destroy pipeline=%p",
		 (void *)pipeline);
	log_append((struct gpu_call_record){
		.type = GPU_CALL_PIPELINE_DESTROY,
	});
}

static void null_cmd_set_pipeline(gpu_cmd_buf_t cmd, gpu_pipeline_t pipeline)
{
	LOG_INFO("renderer_null: cmd_set_pipeline cmd=%p pipeline=%p",
		 (void *)cmd, (void *)pipeline);
	log_append((struct gpu_call_record){
		.type = GPU_CALL_CMD_SET_PIPELINE,
	});
}

static void null_cmd_begin_render_pass(gpu_cmd_buf_t cmd,
				       const struct gpu_render_pass_desc *desc)
{
	LOG_INFO("renderer_null: cmd_begin_render_pass cmd=%p color_count=%u",
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
	LOG_INFO("renderer_null: cmd_end_render_pass cmd=%p", (void *)cmd);
	log_append((struct gpu_call_record){
		.type = GPU_CALL_CMD_END_RENDER_PASS,
	});
}

static void null_cmd_barrier(gpu_cmd_buf_t cmd,
			      const struct gpu_barrier *barriers,
			      uint32_t count)
{
	(void)barriers;
	LOG_INFO("renderer_null: cmd_barrier cmd=%p count=%u",
		 (void *)cmd, count);
	log_append((struct gpu_call_record){
		.type = GPU_CALL_CMD_BARRIER,
		.args = { .cmd_barrier = { .count = count } },
	});
}

static void null_cmd_draw_indexed(gpu_cmd_buf_t cmd,
				  const struct gpu_draw_indexed_args *args,
				  void *data_gpu)
{
	LOG_INFO("renderer_null: cmd_draw_indexed cmd=%p index_count=%u data=%p",
		 (void *)cmd, args->index_count, data_gpu);
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
			      uint32_t x, uint32_t y, uint32_t z,
			      void *data_gpu)
{
	LOG_INFO("renderer_null: cmd_dispatch cmd=%p x=%u y=%u z=%u data=%p",
		 (void *)cmd, x, y, z, data_gpu);
	log_append((struct gpu_call_record){
		.type = GPU_CALL_CMD_DISPATCH,
		.args = { .cmd_dispatch = { .x = x, .y = y, .z = z } },
	});
}

static void *null_gpu_malloc(size_t size)
{
	LOG_INFO("renderer_null: gpu_malloc size=%zu", size);
	log_append((struct gpu_call_record){
		.type = GPU_CALL_GPU_MALLOC,
		.args = { .gpu_malloc = { .size = size } },
	});
	return NULL;
}

static void null_gpu_free(void *ptr)
{
	LOG_INFO("renderer_null: gpu_free ptr=%p", ptr);
	log_append((struct gpu_call_record){ .type = GPU_CALL_GPU_FREE });
}

static void *null_gpu_host_to_device_ptr(void *host_ptr)
{
	LOG_INFO("renderer_null: gpu_host_to_device_ptr ptr=%p", host_ptr);
	log_append((struct gpu_call_record){
		.type = GPU_CALL_GPU_HOST_TO_DEVICE_PTR,
	});
	return host_ptr;
}

static gpu_texture_t
null_texture_create(const struct gpu_texture_desc *desc)
{
	LOG_INFO("renderer_null: texture_create fmt=%u w=%u h=%u",
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
	LOG_INFO("renderer_null: texture_destroy texture=%p", (void *)texture);
	log_append((struct gpu_call_record){
		.type = GPU_CALL_TEXTURE_DESTROY,
	});
}

static const struct gpu_api null_api = {
	.cmd_buf_begin          = null_cmd_buf_begin,
	.cmd_buf_submit         = null_cmd_buf_submit,
	.pipeline_create        = null_pipeline_create,
	.pipeline_destroy       = null_pipeline_destroy,
	.cmd_set_pipeline       = null_cmd_set_pipeline,
	.cmd_begin_render_pass  = null_cmd_begin_render_pass,
	.cmd_end_render_pass    = null_cmd_end_render_pass,
	.cmd_barrier            = null_cmd_barrier,
	.cmd_draw_indexed       = null_cmd_draw_indexed,
	.cmd_dispatch           = null_cmd_dispatch,
	.gpu_malloc             = null_gpu_malloc,
	.gpu_free               = null_gpu_free,
	.gpu_host_to_device_ptr = null_gpu_host_to_device_ptr,
	.texture_create         = null_texture_create,
	.texture_destroy        = null_texture_destroy,
};

static void renderer_null_init(void)
{
	LOG_INFO("renderer_null: init");
	renderer_null_reset_log();
}

static void renderer_null_shutdown(void)
{
	LOG_INFO("renderer_null: shutdown");
}

static const struct subsystem desc = {
	.name     = "renderer",
	.api      = &null_api,
	.init     = renderer_null_init,
	.shutdown = renderer_null_shutdown,
};

void plugin_entry(struct subsystem_manager *mgr)
{
	subsystem_manager_register(mgr, &desc);
}

const struct gpu_call_record *renderer_null_get_log(uint32_t *out_count)
{
	*out_count = g_log_count;
	return g_log;
}

void renderer_null_reset_log(void)
{
	g_log_count = 0;
}
