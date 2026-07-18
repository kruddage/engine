/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "engine.h"
#include "subsystem_manager.h"

#include "log.h"
#include "memory.h"
#include "memory_api.h"
#include "script.h"
#include "stats_api.h"
#include "version.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
/* The Scheme image, baked into the module at build time (krudd-embed-file). */
#include "runtime_scm.h"
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
	{ .name = "log",    .api = &g_log_api, .init = log_init, .shutdown = log_shutdown },
	{ .name = "memory", .api = &g_mem_api, .init = mem_init, .shutdown = mem_shutdown },
	{ .name = "stats",  .api = &g_stats_api                                           },
	{ NULL }
};

#ifdef __EMSCRIPTEN__
/*
 * Every plugin is compiled into this single WASM module (there are no side
 * modules and no dynamic loading); each exposes a unique <name>_plugin_entry
 * that registers its subsystem. subsystem_manager_register() runs a subsystem's
 * init at register time, so calling these in dependency order boots each plugin
 * after the services it depends on — the order the old dlopen chain enforced.
 */
void asset_plugin_entry(struct subsystem_manager *mgr);
void edit_plugin_entry(struct subsystem_manager *mgr);
void entity_plugin_entry(struct subsystem_manager *mgr);
void renderer_webgl_plugin_entry(struct subsystem_manager *mgr);
void renderer_webgpu_plugin_entry(struct subsystem_manager *mgr);
int  renderer_webgpu_device_ready(void);
void renderer_webgpu_release_frame(void);
void fg_plugin_entry(struct subsystem_manager *mgr);
void scene_renderer_plugin_entry(struct subsystem_manager *mgr);
void kruddboard_plugin_entry(struct subsystem_manager *mgr);
void kruddgui_plugin_entry(struct subsystem_manager *mgr);
void audio_scriptnode_plugin_entry(struct subsystem_manager *mgr);
void demo_plugin_entry(struct subsystem_manager *mgr);
void tictactoe_plugin_entry(struct subsystem_manager *mgr);

/*
 * The boot order, as data so the profiler can time each entry point and label
 * its slice. Same sequence the hand-unrolled calls used to run in — services
 * before the plugins that consume them.
 */
static const struct {
	const char *name;
	void      (*entry)(struct subsystem_manager *);
} plugin_table[] = {
	{ "asset",          asset_plugin_entry          },
	{ "audio",          audio_scriptnode_plugin_entry },
	{ "edit",           edit_plugin_entry           },
	{ "entity",         entity_plugin_entry         },
	{ "renderer_webgl", renderer_webgl_plugin_entry },
	{ "fg",             fg_plugin_entry             },
	{ "scene_renderer", scene_renderer_plugin_entry },
	{ "kruddboard",     kruddboard_plugin_entry     },
	{ "kruddgui",       kruddgui_plugin_entry       },
	/*
	 * Built-in games register last: they resolve the "scene" api (entity
	 * plugin) and register on the launcher, which needs the asset catalog the
	 * asset plugin seeded, so both must already be up. Registration order is
	 * launcher-button order — the demo leads, then the games.
	 */
	{ "demo",           demo_plugin_entry           },
	{ "tictactoe",      tictactoe_plugin_entry      },
};
#endif

static struct subsystem_manager manager;
static int32_t frame_count;

#ifdef __EMSCRIPTEN__
/* Set while the WebGPU path waits on its device before finishing the boot. */
static int g_webgpu_boot_pending;

#endif

#ifdef __EMSCRIPTEN__
static double s_last_ms;
static float  s_frame_times[60];
static int    s_ft_head;
static double s_boot_ms;	/* emscripten_get_now() at engine_init entry */

/*
 * Record how long the slice since START took as the next startup phase.
 * Overflow past STATS_MAX_PHASES is dropped rather than clobbering earlier
 * phases — the bounded table keeps the profiler allocation-free.
 */
static void stats_record_phase(const char *name, double start)
{
	uint32_t n = g_stats_api.phase_count;

	if (n >= STATS_MAX_PHASES)
		return;
	g_stats_api.phases[n].name = name;
	g_stats_api.phases[n].ms   = (float)(emscripten_get_now() - start);
	g_stats_api.phase_count    = n + 1;
}

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

#ifdef __EMSCRIPTEN__
/*
 * Flip the shell's status pill from "loading" to "running". kruddSetRunning is
 * defined in shell.html; the typeof guard keeps this safe if the shell changes.
 */
EM_JS(void, krudd_signal_running, (void), {
	if (typeof window.kruddSetRunning === 'function')
		window.kruddSetRunning();
})

/*
 * Tell the shell the first frame is on screen, so it can arm the launcher.
 * The launcher overlay is painted from static HTML while the module is still
 * downloading, but a game can only be loaded once the subsystems are up and the
 * scene has actually rendered — picking one before then builds into a world the
 * renderer has not framed yet. Gating on the first tick, not on init, is what
 * makes "impatient" clicks wait for a live engine rather than bug out.
 */
EM_JS(void, krudd_signal_ready, (void), {
	if (typeof window.kruddSetReady === 'function')
		window.kruddSetReady();
})

/*
 * Whether the page should boot the WebGPU backend. WebGPU is the default
 * while the port chases WebGL parity — pass ?renderer=webgl to opt out back
 * to the established path, and Firefox opts out unconditionally until its
 * WebGPU implementation is further along. See finish_plugin_boot for what
 * selecting WebGPU actually defers (the render cluster and games wait on the
 * device handshake).
 *
 * The decision itself lives in the shell (window.kruddWantsWebGPU in
 * shell.html.in), not here, so the launcher-hiding check and this backend
 * selection can never disagree about which renderer is about to boot.
 */
EM_JS(int, krudd_wants_webgpu, (void), {
	try {
		return window.kruddWantsWebGPU() ? 1 : 0;
	} catch (e) {
		return 1;
	}
})
#endif

#ifdef __EMSCRIPTEN__
/*
 * Register the remaining plugins and load the runtime image. Runs inline on the
 * GL path and from the tick on the WebGPU path, once the device exists — the
 * plugins are the same either way, so both paths go through here rather than
 * growing two boot orders that could drift apart.
 *
 * renderer_webgl is skipped when WebGPU is driving: both register under the
 * name "renderer", and the backend already registered itself.
 */
static void finish_plugin_boot(int webgpu)
{
	size_t i;
	double phase;

	/* The frame graph is about to own the backbuffer; the probe must stop
	 * clearing it. Before the loop, so no tick can land in between. */
	if (webgpu)
		renderer_webgpu_release_frame();

	for (i = 0; i < sizeof(plugin_table) / sizeof(plugin_table[0]); i++) {
		if (webgpu && strcmp(plugin_table[i].name, "renderer_webgl") == 0)
			continue;
		phase = emscripten_get_now();
		plugin_table[i].entry(&manager);
		stats_record_phase(plugin_table[i].name, phase);
	}

	/* Load the runtime image: it owns the body of the frame from here. */
	phase = emscripten_get_now();
	script_eval(RUNTIME_SCM);
	stats_record_phase("runtime_scm", phase);

	g_stats_api.init_ms = (float)(emscripten_get_now() - s_boot_ms);
}
#endif

void engine_init(void)
{
#ifdef __EMSCRIPTEN__
	double phase;

	s_boot_ms = emscripten_get_now();
#endif
	subsystem_manager_init(&manager, subsystems);
	frame_count = 0;
	LOG_INFO("engine: init " ENGINE_VERSION_STRING);
#ifdef __EMSCRIPTEN__
	/*
	 * Boot the Scheme interpreter first: script_init loads the shader
	 * transpiler into the image, and the renderer plugins lower their
	 * shaders through it as they build pipelines at register time.
	 */
	phase = emscripten_get_now();
	script_init();
	stats_record_phase("script_init", phase);

	/*
	 * Register the statically-linked plugins now that core services exist,
	 * timing each entry point so the board can show where boot went — a
	 * plugin lowers its shaders and bakes its textures at register time, so
	 * this is where "adding a texture" shows up as startup cost.
	 */
	if (krudd_wants_webgpu()) {
		/*
		 * WebGPU boots in two phases. Every plugin below the backend
		 * creates GPU resources in its init — the scene renderer builds
		 * pipelines and textures there — but the WebGPU device only
		 * arrives through the adapter/device callback chain, long after
		 * this function returns. So register the backend alone here and
		 * let engine_tick finish the boot once the device exists.
		 *
		 * The GL path is untouched: it has a device the moment its
		 * context is created, so it still boots straight through.
		 */
		phase = emscripten_get_now();
		renderer_webgpu_plugin_entry(&manager);
		stats_record_phase("renderer_webgpu", phase);
		g_webgpu_boot_pending = 1;
	} else {
		finish_plugin_boot(0);
	}
#endif
}

void engine_tick(void)
{
	frame_count++;
#ifdef __EMSCRIPTEN__
	/*
	 * Finish the WebGPU boot the moment the device lands. Until then the only
	 * registered subsystem is the backend itself, whose tick pumps the
	 * instance so those callbacks can resolve — so this is the one thing that
	 * has to run before the runtime image exists.
	 */
	if (g_webgpu_boot_pending && renderer_webgpu_device_ready()) {
		g_webgpu_boot_pending = 0;
		finish_plugin_boot(1);
	}

	/*
	 * Time to first frame, captured once on the opening tick, two ways:
	 *
	 * first_frame_ms subtracts s_boot_ms, so it's init plus the browser's
	 * first animation-frame round-trip — "engine boot" measured from
	 * engine_init entry, separated from "waiting on the first rAF".
	 *
	 * page_to_first_frame_ms is the same instant left raw. emscripten_get_now()
	 * is performance.now() (ms since navigation start), so the unsubtracted
	 * value is the full page-load-to-first-frame wall clock — it keeps the
	 * download + WASM compile + runtime bring-up that runs before main(),
	 * which first_frame_ms throws away. This is what tracks the seconds spent
	 * on a black screen; the 38 ms figures never saw that span at all.
	 */
	if (frame_count == 1) {
		double now = emscripten_get_now();

		g_stats_api.first_frame_ms         = (float)(now - s_boot_ms);
		g_stats_api.page_to_first_frame_ms = (float)now;
	}
	stats_update();
	/* The runtime image loads with the rest of the boot, so there is nothing
	 * to tick while the WebGPU path is still waiting on its device. */
	if (!g_webgpu_boot_pending)
		script_tick();
#endif
	if (frame_count % 60 == 0)
		LOG_DEBUG("engine: frame %d", frame_count);
	subsystem_manager_tick(&manager);
#ifdef __EMSCRIPTEN__
	/* The first tick has now rendered a frame: arm the launcher (see
	 * krudd_signal_ready). Runs after the render so a click that follows
	 * lands on a live, framed engine. */
	if (frame_count == 1)
		krudd_signal_ready();
#endif
}

void engine_shutdown(void)
{
	LOG_INFO("engine: shutdown");
#ifdef __EMSCRIPTEN__
	script_shutdown();
#endif
	subsystem_manager_shutdown(&manager);
}

int main(void)
{
	engine_init();
#ifdef __EMSCRIPTEN__
	krudd_signal_running();
	emscripten_set_main_loop(engine_tick, 0, 1);
#else
	/* Native main loop not yet implemented; seam for future loop. */
#endif
	engine_shutdown();
	return 0;
}
