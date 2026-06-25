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
	assert(gpu->cmd_begin_render_pass != NULL);
	assert(gpu->cmd_end_render_pass != NULL);
	assert(gpu->cmd_barrier != NULL);
	assert(gpu->cmd_draw_indexed != NULL);
	assert(gpu->cmd_dispatch != NULL);
	assert(gpu->gpu_malloc != NULL);
	assert(gpu->gpu_free != NULL);
	assert(gpu->gpu_host_to_device_ptr != NULL);
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
	gpu->cmd_begin_render_pass(NULL, &rpdesc);
	gpu->cmd_end_render_pass(NULL);
	gpu->cmd_barrier(NULL, &barrier, 1);
	gpu->cmd_draw_indexed(NULL, &draw, NULL);
	gpu->cmd_dispatch(NULL, 1, 1, 1, NULL);
	gpu->gpu_malloc(64);
	gpu->gpu_free(NULL);
	gpu->gpu_host_to_device_ptr(NULL);
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
	};
	struct gpu_draw_indexed_args draw = {
		.index_count    = 6,
		.instance_count = 4,
	};
	uint32_t count;

	setup();
	gpu = (const struct gpu_api *)subsystem_manager_get_api(&mgr, "renderer");

	gpu->pipeline_create(&pdesc);
	gpu->cmd_draw_indexed(NULL, &draw, NULL);

	log = renderer_null_get_log(&count);
	assert(count == 2);
	assert(log[0].type == GPU_CALL_PIPELINE_CREATE);
	assert(log[0].args.pipeline_create.color_format_count == 2);
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
	RUN(caps_flags);

	printf("%d/%d tests passed\n", tests_passed, tests_run);
	return tests_passed == tests_run ? 0 : 1;
}
