/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "world.h"
#include "entity_api.h"
#include "entity_script.h"
#include "scene_script.h"
#include "scene.h"
#include "scene_edit.h"
#include "edit_api.h"
#include "asset_api.h"
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
static const struct asset_api       *g_asset;  /* NULL = script source unavailable */
static const struct memory_api      *g_mem;
static const struct stats_api       *g_stats;
static const struct edit_api        *g_edit;   /* NULL = undo unavailable */

/* Seconds since the scene subsystem started, the clock entity scripts read. */
static float                         g_clock;

/* True skips world_tick + entity scripts for the frame (editor "Paused"). */
static int32_t                       g_paused;

/* True when id names a live entity — the precondition for a recordable edit. */
static int32_t entity_is_live(int32_t id)
{
	return id >= 0 && (uint32_t)id < g_world.count && g_world.alive[id];
}

/* Snapshot the world before an edit, but only when there's a history to feed. */
static struct world_snapshot *edit_before(void)
{
	return (g_edit && g_mem) ? world_snapshot_capture(&g_world, g_mem)
				 : NULL;
}

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

/*
 * Build a (scene ...) Scheme form into the live world (see scene_script.c). A
 * boot-time populate, not an editor edit, so it bypasses the undo history — the
 * scene is the starting state, not a recordable change on top of one.
 */
static int32_t scene_build_scm(const char *src)
{
	return scene_script_build(&g_world, g_asset, src);
}

/*
 * Run an image function with the live world bound (see scene_script_call). The
 * event-driven twin of scene_build_scm: a game's Scheme rules mutate the world
 * in response to a click. Not recorded on the undo history — gameplay is not an
 * editor edit.
 */
static int32_t scene_dispatch_scm(const char *fn, int32_t arg)
{
	return scene_script_call(&g_world, g_asset, fn, arg);
}

/* Reset the world to empty (the launcher's "unload current scene"). */
static void scene_clear_world(void)
{
	world_reset(&g_world);
}

static int32_t scene_create_entity(int32_t parent,
				   const struct transform *local,
				   uint32_t mask, uint32_t render_ref)
{
	struct world_snapshot *before = edit_before();
	int32_t                id;

	id = world_create_entity(&g_world, parent, local, mask);
	if (id >= 0 && render_ref)
		world_set_render_ref(&g_world, id, render_ref);

	/* Only a real creation is recordable; a failed create changed nothing. */
	if (id >= 0) {
		scene_edit_record(g_edit, g_mem, &g_world, before,
				  "Create Entity", 0);
	} else {
		world_snapshot_free(before, g_mem);
	}
	return id;
}

static void scene_destroy_entity(int32_t id)
{
	int32_t                live   = entity_is_live(id);
	struct world_snapshot *before = live ? edit_before() : NULL;

	world_destroy_entity(&g_world, id);
	if (live) {
		scene_edit_record(g_edit, g_mem, &g_world, before,
				  "Delete Entity", 0);
	}
}

static void scene_set_transform(int32_t id, const struct transform *local)
{
	int32_t                live   = entity_is_live(id);
	struct world_snapshot *before = live ? edit_before() : NULL;

	world_set_transform(&g_world, id, local);
	if (live) {
		scene_edit_record(g_edit, g_mem, &g_world, before, "Move Entity",
				  scene_edit_key(id, SCENE_EDIT_TRANSFORM));
	}
}

static void scene_set_name(int32_t id, const char *name)
{
	struct world_snapshot *before = edit_before();

	/* Record only when the rename actually took (0), not on overflow (-1). */
	if (world_set_name(&g_world, id, name) == 0) {
		scene_edit_record(g_edit, g_mem, &g_world, before,
				  "Rename Entity",
				  scene_edit_key(id, SCENE_EDIT_NAME));
	} else {
		world_snapshot_free(before, g_mem);
	}
}

static void scene_set_render_ref(int32_t id, uint32_t render_ref)
{
	int32_t                live   = entity_is_live(id);
	struct world_snapshot *before = live ? edit_before() : NULL;

	world_set_render_ref(&g_world, id, render_ref);
	if (live) {
		scene_edit_record(g_edit, g_mem, &g_world, before, "Bind Mesh",
				  scene_edit_key(id, SCENE_EDIT_RENDER));
	}
}

static void scene_set_material_ref(int32_t id, uint32_t material_ref)
{
	int32_t                live   = entity_is_live(id);
	struct world_snapshot *before = live ? edit_before() : NULL;

	world_set_material_ref(&g_world, id, material_ref);
	if (live) {
		scene_edit_record(g_edit, g_mem, &g_world, before, "Bind Material",
				  scene_edit_key(id, SCENE_EDIT_MATERIAL));
	}
}

static void scene_set_script_ref(int32_t id, uint32_t script_ref)
{
	int32_t                live   = entity_is_live(id);
	struct world_snapshot *before = live ? edit_before() : NULL;

	world_set_script_ref(&g_world, id, script_ref);
	if (live) {
		scene_edit_record(g_edit, g_mem, &g_world, before, "Bind Script",
				  scene_edit_key(id, SCENE_EDIT_SCRIPT));
	}
}

static void scene_set_script_params(int32_t id, const uint8_t *bytes,
				    uint32_t len)
{
	int32_t                live   = entity_is_live(id);
	struct world_snapshot *before = live ? edit_before() : NULL;

	world_set_script_params(&g_world, id, bytes, len);
	if (live) {
		scene_edit_record(g_edit, g_mem, &g_world, before,
				  "Edit Script Params",
				  scene_edit_key(id, SCENE_EDIT_SCRIPT_PARAMS));
	}
}

static void scene_set_material_params(int32_t id, const uint8_t *bytes,
				      uint32_t len)
{
	int32_t                live   = entity_is_live(id);
	struct world_snapshot *before = live ? edit_before() : NULL;

	world_set_material_params(&g_world, id, bytes, len);
	if (live) {
		scene_edit_record(g_edit, g_mem, &g_world, before,
				  "Edit Material Params",
				  scene_edit_key(id, SCENE_EDIT_MATERIAL_PARAMS));
	}
}

static void scene_set_mesh_params(int32_t id, const uint8_t *bytes,
				  uint32_t len)
{
	int32_t                live   = entity_is_live(id);
	struct world_snapshot *before = live ? edit_before() : NULL;

	world_set_mesh_params(&g_world, id, bytes, len);
	if (live) {
		scene_edit_record(g_edit, g_mem, &g_world, before,
				  "Edit Mesh Params",
				  scene_edit_key(id, SCENE_EDIT_MESH_PARAMS));
	}
}

static void scene_set_texture_params(int32_t id, const uint8_t *bytes,
				     uint32_t len)
{
	int32_t                live   = entity_is_live(id);
	struct world_snapshot *before = live ? edit_before() : NULL;

	world_set_texture_params(&g_world, id, bytes, len);
	if (live) {
		scene_edit_record(g_edit, g_mem, &g_world, before,
				  "Edit Texture Params",
				  scene_edit_key(id, SCENE_EDIT_TEXTURE_PARAMS));
	}
}

static int32_t scene_get_selected(void)
{
	return world_get_selected(&g_world);
}

static void scene_set_selected(int32_t id)
{
	world_set_selected(&g_world, id);
}

static int32_t scene_get_outline(void)
{
	return world_get_outline(&g_world);
}

static void scene_set_outline(int32_t id)
{
	world_set_outline(&g_world, id);
}

static int32_t scene_get_paused(void)
{
	return g_paused;
}

static void scene_set_paused(int32_t paused)
{
	g_paused = paused ? 1 : 0;
}

static const struct entity_api g_entity_api = {
	.get_world      = scene_get_world,
	.load_scene     = scene_load,
	.build_scene_scm = scene_build_scm,
	.dispatch_scm   = scene_dispatch_scm,
	.clear_world    = scene_clear_world,
	.create_entity  = scene_create_entity,
	.destroy_entity = scene_destroy_entity,
	.set_transform  = scene_set_transform,
	.set_name       = scene_set_name,
	.set_render_ref = scene_set_render_ref,
	.set_material_ref = scene_set_material_ref,
	.set_script_ref = scene_set_script_ref,
	.set_script_params = scene_set_script_params,
	.set_material_params = scene_set_material_params,
	.set_mesh_params = scene_set_mesh_params,
	.set_texture_params = scene_set_texture_params,
	.get_selected   = scene_get_selected,
	.set_selected   = scene_set_selected,
	.get_outline    = scene_get_outline,
	.set_outline    = scene_set_outline,
	.get_paused     = scene_get_paused,
	.set_paused     = scene_set_paused,
};

/*
 * Start with an empty world. Requesting a scene is the host application's
 * job — it loads one through the entity_api load_scene() entry (e.g. from
 * local storage) when it has one to show. This plugin owns ingest and the
 * per-frame tick, not asset acquisition, so it fetches nothing at init.
 */
static void scene_init(void)
{
	world_reset(&g_world);
	g_clock  = 0.0f;
	g_paused = 0;
	/* Register the entity-* primitives so bound scripts can drive entities. */
	entity_script_init();
	/* Register the scene-* primitives so a (scene ...) form can build a world. */
	scene_script_init();
}

static void scene_tick(void)
{
	/* tick() takes no args; read the frame delta from the "stats" api. */
	float dt = g_stats ? g_stats->last_frame_ms : 0.0f;

	if (g_paused)
		return;

	world_tick(&g_world, dt);

	/*
	 * Run scripts after propagation: world_tick has just refilled
	 * world_xform from the authored transforms, so a script reads its rest
	 * pose (local) and overwrites this frame's render pose (world_xform)
	 * without ever touching the authored data. The clock is seconds, so
	 * time-based animation reads naturally (the stats delta is milliseconds).
	 */
	g_clock += dt / 1000.0f;
	entity_script_tick(&g_world, g_asset, g_clock);
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

void entity_plugin_entry(struct subsystem_manager *mgr)
{
	/* Resolve service vtables before register(), which calls init at once. */
	g_codec = subsystem_manager_get_api(mgr, "asset_codec");
	g_asset = subsystem_manager_get_api(mgr, "asset");
	g_mem   = subsystem_manager_get_api(mgr, "memory");
	g_stats = subsystem_manager_get_api(mgr, "stats");
	g_edit  = subsystem_manager_get_api(mgr, "edit");
	subsystem_manager_register(mgr, &scene_desc);
}
