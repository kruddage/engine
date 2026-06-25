/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "engine.h"
#include "subsystem_manager.h"

#include "log.h"
#include "memory.h"
#include "memory_api.h"
#include "plugin_loader.h"
#include "stats_api.h"
#include "version.h"

#include <stdint.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

/*
 * Vtables for the engine's own services.  Taking the address of each function
 * here keeps them reachable from main(), preventing dead-code elimination from
 * removing them before the dynamic linker can expose them to plugins.  Plugins
 * access these through subsystem_manager_get_api() and call via the pointer —
 * no direct named import from the main module required.
 */
static const struct log_api g_log_api = {
	.write       = log_write,
	.get_history = log_get_history,
};

static const struct memory_api g_mem_api = {
	.alloc        = mem_alloc,
	.alloc_zero   = mem_alloc_zero,
	.free         = mem_free,
	.pool_create  = mem_pool_create,
	.pool_alloc   = mem_pool_alloc,
	.pool_free    = mem_pool_free,
	.pool_destroy = mem_pool_destroy,
};

/* Mutable; updated each tick and read by plugins via the "stats" api. */
static struct stats_api g_stats_api;

static const struct subsystem subsystems[] = {
	{ .name = "log",           .api = &g_log_api, .init = log_init,           .shutdown = log_shutdown           },
	{ .name = "memory",        .api = &g_mem_api, .init = mem_init,           .shutdown = mem_shutdown           },
	{ .name = "plugin_loader",                    .init = plugin_loader_init, .shutdown = plugin_loader_shutdown },
	{ .name = "stats",         .api = &g_stats_api                                                                },
	{ NULL }
};

static const char * const plugins[] = {
	"hello_plugin.wasm",
	"asset_plugin.wasm",
	"renderer_webgl.wasm",
	"imgui_plugin.wasm",
	"kruddboard.wasm",
	NULL,
};

static struct subsystem_manager manager;
static int32_t frame_count;

#ifdef __EMSCRIPTEN__
static double s_last_ms;
static float  s_frame_times[60];
static int    s_ft_head;

static void stats_update(void)
{
	double now;
	float  dt;
	float  sum;
	int    i;

	now = emscripten_get_now();
	if (s_last_ms == 0.0) {
		s_last_ms = now;
		return;
	}

	dt        = (float)(now - s_last_ms);
	s_last_ms = now;

	s_frame_times[s_ft_head] = dt;
	s_ft_head = (s_ft_head + 1) % 60;

	sum = 0.0f;
	for (i = 0; i < 60; i++)
		sum += s_frame_times[i];

	g_stats_api.last_frame_ms = dt;
	g_stats_api.fps_avg       = sum > 0.0f ? 60000.0f / sum : 0.0f;
	g_stats_api.frame_count   = (uint32_t)frame_count;
}
#endif

void engine_init(void)
{
	plugin_loader_set_manager(&manager);
	plugin_loader_set_plugins(plugins);
	subsystem_manager_init(&manager, subsystems);
	frame_count = 0;
	LOG_INFO("engine: init v" ENGINE_VERSION_STRING);
}

void engine_tick(void)
{
	frame_count++;
#ifdef __EMSCRIPTEN__
	stats_update();
#endif
	if (frame_count % 60 == 0)
		LOG_DEBUG("engine: frame %d", frame_count);
	subsystem_manager_tick(&manager);
}

void engine_shutdown(void)
{
	LOG_INFO("engine: shutdown");
	subsystem_manager_shutdown(&manager);
}

int main(void)
{
	engine_init();
#ifdef __EMSCRIPTEN__
	emscripten_set_main_loop(engine_tick, 0, 1);
#else
	/* Native main loop not yet implemented; seam for future loop. */
#endif
	engine_shutdown();
	return 0;
}
