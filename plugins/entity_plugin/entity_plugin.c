/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "world.h"
#include "entity_api.h"
#include "scene.h"
#include "asset_codec_api.h"
#include "memory_api.h"
#include "stats_api.h"
#include "subsystem_manager.h"

#include <stddef.h>
#include <stdint.h>

/*
 * The runtime world is one large static instance, not a heap allocation: its
 * lifetime is the whole program and other plugins must reach it through the
 * entity_api vtable, never by importing this symbol.
 */
static struct world                  g_world;
static const struct asset_codec_api *g_codec;
static const struct memory_api      *g_mem;
static const struct stats_api       *g_stats;

/*
 * Path the "scene" subsystem ingests at init. A missing or undecodable asset
 * leaves the world empty rather than failing — requesting the asset is the
 * host application's job; this plugin only owns ingest and the per-frame tick.
 */
#define DEFAULT_SCENE_PATH "main.scene"

/* The decoder hands back three caller-owned allocations; release all three. */
static void free_scene(struct scene *s)
{
	if (!s || !g_mem)
		return;
	g_mem->free(s->entities);
	g_mem->free(s->names);
	g_mem->free(s);
}

static int32_t scene_load(const char *path)
{
	struct scene *s;
	int32_t       rc;

	if (!g_codec)
		return -1;
	s = g_codec->get_typed(path);
	if (!s)
		return -1;
	rc = world_ingest_scene(&g_world, s);
	free_scene(s);
	return rc;
}

static const struct world *scene_get_world(void)
{
	return &g_world;
}

static int32_t scene_create_entity(int32_t parent,
				   const struct transform *local,
				   uint32_t mask, uint32_t render_ref)
{
	int32_t id = world_create_entity(&g_world, parent, local, mask);

	if (id >= 0 && render_ref)
		world_set_render_ref(&g_world, id, render_ref);
	return id;
}

static void scene_destroy_entity(int32_t id)
{
	world_destroy_entity(&g_world, id);
}

static void scene_set_transform(int32_t id, const struct transform *local)
{
	world_set_transform(&g_world, id, local);
}

static void scene_set_name(int32_t id, const char *name)
{
	world_set_name(&g_world, id, name);
}

static int32_t scene_get_selected(void)
{
	return world_get_selected(&g_world);
}

static void scene_set_selected(int32_t id)
{
	world_set_selected(&g_world, id);
}

static const struct entity_api g_entity_api = {
	.get_world      = scene_get_world,
	.load_scene     = scene_load,
	.create_entity  = scene_create_entity,
	.destroy_entity = scene_destroy_entity,
	.set_transform  = scene_set_transform,
	.set_name       = scene_set_name,
	.get_selected   = scene_get_selected,
	.set_selected   = scene_set_selected,
};

static void scene_init(void)
{
	world_reset(&g_world);
	scene_load(DEFAULT_SCENE_PATH);
}

static void scene_tick(void)
{
	/* tick() takes no args; read the frame delta from the "stats" api. */
	float dt = g_stats ? g_stats->last_frame_ms : 0.0f;

	world_tick(&g_world, dt);
}

static void scene_shutdown(void)
{
	world_reset(&g_world);
}

static const struct subsystem scene_desc = {
	.name     = "scene",
	.api      = &g_entity_api,
	.init     = scene_init,
	.tick     = scene_tick,
	.shutdown = scene_shutdown,
};

#ifdef __EMSCRIPTEN__
void plugin_entry(struct subsystem_manager *mgr)
#else
void entity_plugin_entry(struct subsystem_manager *mgr)
#endif
{
	/* Resolve service vtables before register(), which calls init at once. */
	g_codec = subsystem_manager_get_api(mgr, "asset_codec");
	g_mem   = subsystem_manager_get_api(mgr, "memory");
	g_stats = subsystem_manager_get_api(mgr, "stats");
	subsystem_manager_register(mgr, &scene_desc);
}
