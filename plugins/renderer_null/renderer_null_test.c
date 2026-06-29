/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "renderer.h"
#include "renderer_null.h"
#include "subsystem_manager.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

static int tests_run;
static int tests_passed;

#define RUN(name) do { \
	tests_run++; \
	test_##name(); \
	tests_passed++; \
	printf("PASS: " #name "\n"); \
} while (0)

void renderer_null_plugin_entry(struct subsystem_manager *mgr);

static const struct subsystem empty_table[] = {{ NULL }};
static struct subsystem_manager mgr;

static void setup(void)
{
	subsystem_manager_init(&mgr, empty_table);
	renderer_null_plugin_entry(&mgr);
	renderer_null_reset_log();
}

static void test_registers_as_renderer(void)
{
	setup();
	assert(subsystem_manager_get_api(&mgr, "renderer") != NULL);
}

static void test_vtable_fully_populated(void)
{
	const struct gpu_api *gpu;

	setup();
	gpu = (const struct gpu_api *)subsystem_manager_get_api(&mgr, "renderer");
	assert(gpu != NULL);
	assert(gpu->cmd_buf_begin != NULL);
	assert(gpu->cmd_buf_submit != NULL);
	assert(gpu->pipeline_create != NULL);
	assert(gpu->pipeline_destroy != NULL);
	assert(gpu->cmd_set_pipeline != NULL);
	assert(gpu->buffer_create != NULL);
	assert(gpu->buffer_destroy != NULL);
	assert(gpu->cmd_bind_vertex_buffer != NULL);
	assert(gpu->cmd_bind_index_buffer != NULL);
	assert(gpu->cmd_bind_uniform_buffer != NULL);
	assert(gpu->cmd_begin_render_pass != NULL);
	assert(gpu->cmd_end_render_pass != NULL);
	assert(gpu->cmd_barrier != NULL);
	assert(gpu->cmd_draw_indexed != NULL);
	assert(gpu->cmd_dispatch != NULL);
	assert(gpu->gpu_malloc != NULL);
	assert(gpu->gpu_free != NULL);
	/* Bindless-only; NULL because GPU_CAP_BINDLESS is not set. */
	assert(gpu->gpu_host_to_device_ptr == NULL);
	assert(gpu->texture_create != NULL);
	assert(gpu->texture_destroy != NULL);
}

static void test_cmd_buf_begin_returns_null(void)
{
	const struct gpu_api *gpu;

	setup();
	gpu = (const struct gpu_api *)subsystem_manager_get_api(&mgr, "renderer");
	assert(gpu->cmd_buf_begin() == NULL);
}

static void test_calls_dont_crash(void)
{
	const struct gpu_api *gpu;
	struct gpu_pipeline_desc pdesc = {
		.color_formats      = { GPU_FORMAT_RGBA8_UNORM },
		.color_format_count = 1,
		.depth_format       = GPU_FORMAT_UNKNOWN,
		.topology           = GPU_TOPOLOGY_TRIANGLE_LIST,
		.sample_count       = 1,
		.vert = {
			.src     = "#version 300 es\nvoid main(){}",
			.stage   = GPU_SHADER_STAGE_VERTEX,
			.dialect = GPU_SHADER_DIALECT_GLSL_ES_300,
		},
		.frag = {
			.src     = "#version 300 es\nvoid main(){}",
			.stage   = GPU_SHADER_STAGE_FRAGMENT,
			.dialect = GPU_SHADER_DIALECT_GLSL_ES_300,
		},
	};
	struct gpu_buffer_desc bdesc = {
		.size         = 256,
		.usage        = GPU_BUFFER_USAGE_VERTEX,
		.initial_data = NULL,
	};
	struct gpu_color_attachment att = {
		.texture  = NULL,
		.load_op  = GPU_LOAD_OP_CLEAR,
		.store_op = GPU_STORE_OP_STORE,
		.clear    = { 0.0f, 0.0f, 0.0f, 1.0f },
	};
	struct gpu_render_pass_desc rpdesc = {
		.color       = { att },
		.color_count = 1,
	};
	struct gpu_barrier barrier = {
		.src_stage  = GPU_STAGE_TOP,
		.dst_stage  = GPU_STAGE_FRAGMENT,
		.src_access = GPU_ACCESS_SHADER_WRITE,
		.dst_access = GPU_ACCESS_SHADER_READ,
	};
	struct gpu_draw_indexed_args draw = {
		.index_count    = 3,
		.instance_count = 1,
	};

	setup();
	gpu = (const struct gpu_api *)subsystem_manager_get_api(&mgr, "renderer");

	gpu->cmd_buf_submit(NULL);
	gpu->pipeline_create(&pdesc);
	gpu->pipeline_destroy(NULL);
	gpu->cmd_set_pipeline(NULL, NULL);
	gpu->buffer_create(&bdesc);
	gpu->buffer_destroy(NULL);
	gpu->cmd_bind_vertex_buffer(NULL, 0, NULL, 0);
	gpu->cmd_bind_index_buffer(NULL, NULL, 0, GPU_INDEX_FORMAT_UINT16);
	gpu->cmd_bind_uniform_buffer(NULL, 0, NULL, 0, 64);
	gpu->cmd_begin_render_pass(NULL, &rpdesc);
	gpu->cmd_end_render_pass(NULL);
	gpu->cmd_barrier(NULL, &barrier, 1);
	gpu->cmd_draw_indexed(NULL, &draw);
	gpu->cmd_dispatch(NULL, 1, 1, 1);
	gpu->gpu_malloc(64);
	gpu->gpu_free(NULL);
	gpu->texture_create(&(struct gpu_texture_desc){
		.format       = GPU_FORMAT_RGBA8_UNORM,
		.width        = 256,
		.height       = 256,
		.mip_levels   = 1,
		.sample_count = 1,
	});
	gpu->texture_destroy(NULL);
}

static void test_log_records_call_type(void)
{
	const struct gpu_call_record *log;
	const struct gpu_api *gpu;
	uint32_t count;

	setup();
	gpu = (const struct gpu_api *)subsystem_manager_get_api(&mgr, "renderer");

	gpu->cmd_buf_begin();

	log = renderer_null_get_log(&count);
	assert(count == 1);
	assert(log[0].type == GPU_CALL_CMD_BUF_BEGIN);
}

static void test_log_reset_clears_entries(void)
{
	const struct gpu_call_record *log;
	const struct gpu_api *gpu;
	uint32_t count;

	setup();
	gpu = (const struct gpu_api *)subsystem_manager_get_api(&mgr, "renderer");

	gpu->cmd_buf_begin();
	gpu->cmd_buf_submit(NULL);
	renderer_null_reset_log();

	log = renderer_null_get_log(&count);
	assert(count == 0);
	(void)log;
}

static void test_log_captures_args(void)
{
	const struct gpu_call_record *log;
	const struct gpu_api *gpu;
	struct gpu_pipeline_desc pdesc = {
		.color_formats      = { GPU_FORMAT_RGBA8_UNORM,
					GPU_FORMAT_BGRA8_UNORM },
		.color_format_count = 2,
		.vert = { .src = "vert", .stage = GPU_SHADER_STAGE_VERTEX },
		.frag = { .src = "frag", .stage = GPU_SHADER_STAGE_FRAGMENT },
	};
	struct gpu_draw_indexed_args draw = {
		.index_count    = 6,
		.instance_count = 4,
	};
	uint32_t count;

	setup();
	gpu = (const struct gpu_api *)subsystem_manager_get_api(&mgr, "renderer");

	gpu->pipeline_create(&pdesc);
	gpu->cmd_draw_indexed(NULL, &draw);

	log = renderer_null_get_log(&count);
	assert(count == 2);
	assert(log[0].type == GPU_CALL_PIPELINE_CREATE);
	assert(log[0].args.pipeline_create.color_format_count == 2);
	assert(log[0].args.pipeline_create.has_vert_src == 1);
	assert(log[0].args.pipeline_create.has_frag_src == 1);
	assert(log[1].type == GPU_CALL_CMD_DRAW_INDEXED);
	assert(log[1].args.cmd_draw_indexed.index_count == 6);
	assert(log[1].args.cmd_draw_indexed.instance_count == 4);
}

static void test_log_records_sequence(void)
{
	const struct gpu_call_record *log;
	const struct gpu_api *gpu;
	struct gpu_barrier barriers[3] = { 0 };
	uint32_t count;

	setup();
	gpu = (const struct gpu_api *)subsystem_manager_get_api(&mgr, "renderer");

	gpu->cmd_buf_begin();
	gpu->cmd_barrier(NULL, barriers, 3);
	gpu->cmd_buf_submit(NULL);

	log = renderer_null_get_log(&count);
	assert(count == 3);
	assert(log[0].type == GPU_CALL_CMD_BUF_BEGIN);
	assert(log[1].type == GPU_CALL_CMD_BARRIER);
	assert(log[1].args.cmd_barrier.count == 3);
	assert(log[2].type == GPU_CALL_CMD_BUF_SUBMIT);
}

static void test_caps_flags(void)
{
	const struct gpu_api *gpu;

	setup();
	gpu = (const struct gpu_api *)subsystem_manager_get_api(&mgr, "renderer");
	assert(gpu->caps & GPU_CAP_DRAW_INDEXED);
	assert(gpu->caps & GPU_CAP_COMPUTE);
	assert(!(gpu->caps & GPU_CAP_DRAW_DIRECT));
}

static void test_texture_ops_logged(void)
{
	const struct gpu_call_record *log;
	const struct gpu_api *gpu;
	struct gpu_texture_desc desc = {
		.format       = GPU_FORMAT_RGBA8_UNORM,
		.width        = 512,
		.height       = 256,
		.mip_levels   = 1,
		.sample_count = 1,
	};
	uint32_t count;

	setup();
	gpu = (const struct gpu_api *)subsystem_manager_get_api(&mgr, "renderer");

	gpu->texture_create(&desc);
	gpu->texture_destroy(NULL);

	log = renderer_null_get_log(&count);
	assert(count == 2);
	assert(log[0].type == GPU_CALL_TEXTURE_CREATE);
	assert(log[0].args.texture_create.format == (uint32_t)GPU_FORMAT_RGBA8_UNORM);
	assert(log[0].args.texture_create.width  == 512);
	assert(log[0].args.texture_create.height == 256);
	assert(log[1].type == GPU_CALL_TEXTURE_DESTROY);
}

static void test_buffer_create_destroy_logged(void)
{
	const struct gpu_call_record *log;
	const struct gpu_api *gpu;
	struct gpu_buffer_desc desc = {
		.size         = 1024,
		.usage        = GPU_BUFFER_USAGE_VERTEX | GPU_BUFFER_USAGE_INDEX,
		.initial_data = NULL,
	};
	uint32_t count;

	setup();
	gpu = (const struct gpu_api *)subsystem_manager_get_api(&mgr, "renderer");

	gpu->buffer_create(&desc);
	gpu->buffer_destroy(NULL);

	log = renderer_null_get_log(&count);
	assert(count == 2);
	assert(log[0].type == GPU_CALL_BUFFER_CREATE);
	assert(log[0].args.buffer_create.size == 1024);
	assert(log[0].args.buffer_create.usage ==
	       (GPU_BUFFER_USAGE_VERTEX | GPU_BUFFER_USAGE_INDEX));
	assert(log[1].type == GPU_CALL_BUFFER_DESTROY);
}

static void test_cmd_bind_vertex_buffer_logged(void)
{
	const struct gpu_call_record *log;
	const struct gpu_api *gpu;
	uint32_t count;

	setup();
	gpu = (const struct gpu_api *)subsystem_manager_get_api(&mgr, "renderer");

	gpu->cmd_bind_vertex_buffer(NULL, 2, NULL, 16);

	log = renderer_null_get_log(&count);
	assert(count == 1);
	assert(log[0].type == GPU_CALL_CMD_BIND_VERTEX_BUFFER);
	assert(log[0].args.cmd_bind_vertex_buffer.slot == 2);
	assert(log[0].args.cmd_bind_vertex_buffer.offset == 16);
}

static void test_cmd_bind_index_buffer_logged(void)
{
	const struct gpu_call_record *log;
	const struct gpu_api *gpu;
	uint32_t count;

	setup();
	gpu = (const struct gpu_api *)subsystem_manager_get_api(&mgr, "renderer");

	gpu->cmd_bind_index_buffer(NULL, NULL, 32, GPU_INDEX_FORMAT_UINT32);

	log = renderer_null_get_log(&count);
	assert(count == 1);
	assert(log[0].type == GPU_CALL_CMD_BIND_INDEX_BUFFER);
	assert(log[0].args.cmd_bind_index_buffer.offset == 32);
	assert(log[0].args.cmd_bind_index_buffer.fmt ==
	       (uint32_t)GPU_INDEX_FORMAT_UINT32);
}

static void test_cmd_bind_uniform_buffer_logged(void)
{
	const struct gpu_call_record *log;
	const struct gpu_api *gpu;
	uint32_t count;

	setup();
	gpu = (const struct gpu_api *)subsystem_manager_get_api(&mgr, "renderer");

	gpu->cmd_bind_uniform_buffer(NULL, 1, NULL, 64, 128);

	log = renderer_null_get_log(&count);
	assert(count == 1);
	assert(log[0].type == GPU_CALL_CMD_BIND_UNIFORM_BUFFER);
	assert(log[0].args.cmd_bind_uniform_buffer.slot == 1);
	assert(log[0].args.cmd_bind_uniform_buffer.offset == 64);
	assert(log[0].args.cmd_bind_uniform_buffer.size == 128);
}

static void test_pipeline_records_shader_source(void)
{
	const struct gpu_call_record *log;
	const struct gpu_api *gpu;
	struct gpu_pipeline_desc pdesc = {
		.color_formats      = { GPU_FORMAT_RGBA8_UNORM },
		.color_format_count = 1,
		.vert = {
			.src     = "#version 300 es\nvoid main(){}",
			.stage   = GPU_SHADER_STAGE_VERTEX,
			.dialect = GPU_SHADER_DIALECT_GLSL_ES_300,
		},
		.frag = {
			.src     = "#version 300 es\nvoid main(){}",
			.stage   = GPU_SHADER_STAGE_FRAGMENT,
			.dialect = GPU_SHADER_DIALECT_GLSL_ES_300,
		},
	};
	uint32_t count;

	setup();
	gpu = (const struct gpu_api *)subsystem_manager_get_api(&mgr, "renderer");

	gpu->pipeline_create(&pdesc);

	log = renderer_null_get_log(&count);
	assert(count == 1);
	assert(log[0].type == GPU_CALL_PIPELINE_CREATE);
	assert(log[0].args.pipeline_create.has_vert_src == 1);
	assert(log[0].args.pipeline_create.has_frag_src == 1);
}

int main(void)
{
	RUN(registers_as_renderer);
	RUN(vtable_fully_populated);
	RUN(cmd_buf_begin_returns_null);
	RUN(calls_dont_crash);
	RUN(log_records_call_type);
	RUN(log_reset_clears_entries);
	RUN(log_captures_args);
	RUN(log_records_sequence);
	RUN(texture_ops_logged);
	RUN(buffer_create_destroy_logged);
	RUN(cmd_bind_vertex_buffer_logged);
	RUN(cmd_bind_index_buffer_logged);
	RUN(cmd_bind_uniform_buffer_logged);
	RUN(pipeline_records_shader_source);
	RUN(caps_flags);

	printf("%d/%d tests passed\n", tests_passed, tests_run);
	return tests_passed == tests_run ? 0 : 1;
}
