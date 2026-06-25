/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "renderer.h"
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

void plugin_entry(struct subsystem_manager *mgr);

static const struct subsystem empty_table[] = {{ NULL }};
static struct subsystem_manager mgr;

static void setup(void)
{
	subsystem_manager_init(&mgr, empty_table);
	plugin_entry(&mgr);
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
}

int main(void)
{
	RUN(registers_as_renderer);
	RUN(vtable_fully_populated);
	RUN(cmd_buf_begin_returns_null);
	RUN(calls_dont_crash);

	printf("%d/%d tests passed\n", tests_passed, tests_run);
	return tests_passed == tests_run ? 0 : 1;
}
