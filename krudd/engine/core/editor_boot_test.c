/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * editor_boot_test — the native render cluster boots a non-empty scene.
 *
 * This is the headless, GPU-free proof that editor_boot_cluster() actually
 * stands up the real engine: the SHIPPED asset_plugin (built-in mesh/shader/
 * material/script catalog), the SHIPPED entity_plugin (the world + entity
 * scripts), the real frame graph and the real scene_renderer — all against the
 * recording null backend, which draws no pixels but logs every gpu_api call.
 *
 * scene_renderer_init seeds the built-in demo scene (floor, box, sphere,
 * pyramid, rook + a Sun light + an orbit "Camera") — the same content the web
 * canvas shows on load. So a settled frame must emit exactly one
 * cmd_draw_indexed per live entity that carries BOTH a mesh and a material,
 * which this asserts by cross-checking the null backend's call log against the
 * live world. That closes the loop the Qt window can't close in CI (no GPU):
 * the scene assembly, real built-in assets, real s7 mesh generation and the
 * forward pass are all exercised here; only the Dawn/Qt present glue is not.
 *
 * The Qt shell (krudd_qt.cpp) runs this exact editor_boot_cluster() sequence
 * against the WebGPU/Dawn backend instead of the null one, so a pass here is a
 * pass for everything in the windowed path up to the backend boundary.
 */
#include "editor_boot.h"

#include "renderer_null.h"
#include "entity_api.h"
#include "scene.h"          /* COMPONENT_RENDER / COMPONENT_MATERIAL */
#include "world.h"
#include "stats_api.h"
#include "log.h"
#include "log_api.h"
#include "memory.h"
#include "memory_api.h"
#include "subsystem.h"
#include "subsystem_manager.h"
#include "script.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

void renderer_null_plugin_entry(struct subsystem_manager *mgr);

/* Core services, exactly as engine.c's own table stands them up. */
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

/* Mutable: entity's scene_tick reads last_frame_ms off "stats" for its script
 * delta, so a non-zero value here lets the seeded demo's spinner/orbit scripts
 * advance — the same way the browser's rAF delta drives them on the web. */
static struct stats_api g_stats;

static const struct subsystem subsystems[] = {
	{ .name = "log",    .api = &g_log_api, .init = log_init, .shutdown = log_shutdown },
	{ .name = "memory", .api = &g_mem_api, .init = mem_init, .shutdown = mem_shutdown },
	{ .name = "stats",  .api = &g_stats                                               },
	{ NULL }
};

/* cmd_draw_indexed calls logged since the last reset. */
static uint32_t count_draws(void)
{
	const struct gpu_call_record *log;
	uint32_t count, i, draws = 0;

	log = renderer_null_get_log(&count);
	for (i = 0; i < count; i++)
		if (log[i].type == GPU_CALL_CMD_DRAW_INDEXED)
			draws++;
	return draws;
}

/* Live entities that carry both a mesh and a material — the ones the forward
 * pass draws. An entity with a mesh but no material keeps it (for picking) but
 * does not draw, so this is the exact expected draw count. */
static uint32_t drawable_entities(const struct world *w)
{
	uint32_t i, n = 0;
	const uint32_t want = COMPONENT_RENDER | COMPONENT_MATERIAL;

	if (!w)
		return 0;
	for (i = 0; i < w->count; i++)
		if (w->alive[i] && (w->mask[i] & want) == want)
			n++;
	return n;
}

int main(void)
{
	struct subsystem_manager mgr;
	const struct entity_api *scene;
	const struct world      *w;
	uint32_t                 drawable, draws, frame;

	/* The s7 image first: meshes, shaders, textures and entity scripts are
	 * all lowered through it, so nothing draws without it (editor_boot.h). */
	script_init();

	subsystem_manager_init(&mgr, subsystems);

	/* Backend before the cluster: the null renderer is synchronous (device
	 * ready at register), standing in for the async WebGPU/Dawn boot the Qt
	 * shell does before it calls the same editor_boot_cluster() below. */
	renderer_null_plugin_entry(&mgr);
	editor_boot_cluster(&mgr);

	/* A real frame delta so the seeded scripts (spinner, orbit-camera) run. */
	g_stats.last_frame_ms = 16.0f;

	scene = subsystem_manager_get_api(&mgr, "scene");
	assert(scene && "editor_boot_cluster must register the scene api");
	w = scene->get_world();
	assert(w && "the scene api must expose a world");

	/*
	 * The demo scene must have seeded renderables — this is the "the cluster
	 * booted the real built-in catalog and built a scene" check. If asset
	 * seeding or entity creation had silently no-op'd, this is 0.
	 */
	drawable = drawable_entities(w);
	printf("editor_boot_test: %u drawable entities seeded\n", drawable);
	assert(drawable > 0 && "scene_renderer_init should seed the demo scene");

	/*
	 * Meshes upload lazily on the first tick (ensure_meshes compiles each
	 * (mesh ...) source through s7), so tick a few frames to let the scene
	 * settle before measuring, then measure one clean frame.
	 */
	for (frame = 0; frame < 3; frame++)
		subsystem_manager_tick(&mgr);

	renderer_null_reset_log();
	subsystem_manager_tick(&mgr);

	/* The drawable set can change as scripts move things, but not their
	 * count, so re-read it for the exact expectation this frame. */
	drawable = drawable_entities(scene->get_world());
	draws    = count_draws();
	printf("editor_boot_test: %u draws for %u drawable entities\n",
	       draws, drawable);
	assert(draws == drawable &&
	       "one draw per live mesh+material entity in the forward pass");

	subsystem_manager_shutdown(&mgr);
	mem_shutdown();

	printf("editor_boot tests passed\n");
	return 0;
}
