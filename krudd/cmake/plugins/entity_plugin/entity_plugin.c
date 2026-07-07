/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "world.h"
#include "entity_api.h"
#include "scene.h"
#include "scene_edit.h"
#include "edit_api.h"
#include "asset_codec_api.h"
#include "backend_api.h"
#include "branch_api.h"
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
static const struct edit_api        *g_edit;  /* NULL = undo unavailable */
static struct subsystem_manager     *g_mgr;    /* for lazy backend lookup */
static const struct branch_api      *g_branch; /* NULL = branching off */

/*
 * A recordable scene edit just landed — signal the branching host so it
 * debounces a live-save + auto-snapshot of the active branch (#213).  The
 * backend may register after this plugin and branching is absent on providers
 * without it, so the branch api is resolved lazily and a NULL one is a safe
 * no-op (undo/redo and editing still work without branching).
 */
static void note_scene_edit(void)
{
	if (!g_branch && g_mgr) {
		const struct backend_api *be =
			subsystem_manager_get_api(g_mgr, "backend");

		if (be && (be->get_caps() & BACKEND_CAP_BRANCHING))
			g_branch = be->branching();
	}
	if (g_branch)
		g_branch->mark_dirty();
}

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

/*
 * Serialize the live world to canonical .scene bytes for content-addressing
 * (#214/#235): export to an at-rest struct scene, then hand it to the scene
 * encoder registered on the codec.  The intermediate scene is freed here; the
 * caller owns the returned byte buffer.
 */
static void *scene_export_bytes(uint32_t *out_size)
{
	struct scene *s;
	void         *bytes;

	if (!g_codec || !g_mem)
		return NULL;
	s = world_export_scene(&g_world, g_mem);
	if (!s)
		return NULL;
	bytes = g_codec->encode("scene", s, out_size);
	free_scene(s);
	return bytes;
}

/*
 * Replace the live world with the scene decoded from raw bytes — the atomic
 * reload behind a branch switch / snapshot restore (#215/#216).  Decodes off
 * the bytes directly (not an asset path) via the codec, then ingests.
 */
static int32_t scene_ingest_bytes(const void *bytes, uint32_t size)
{
	struct scene *s;
	int32_t       rc;

	if (!g_codec)
		return -1;
	s = g_codec->decode_bytes("scene", bytes, size);
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
	struct world_snapshot *before = edit_before();
	int32_t                id;

	id = world_create_entity(&g_world, parent, local, mask);
	if (id >= 0 && render_ref)
		world_set_render_ref(&g_world, id, render_ref);

	/* Only a real creation is recordable; a failed create changed nothing. */
	if (id >= 0) {
		scene_edit_record(g_edit, g_mem, &g_world, before,
				  "Create Entity", 0);
		note_scene_edit();
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
		note_scene_edit();
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
		note_scene_edit();
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
		note_scene_edit();
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
		note_scene_edit();
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

static const struct entity_api g_entity_api = {
	.get_world      = scene_get_world,
	.load_scene     = scene_load,
	.create_entity  = scene_create_entity,
	.destroy_entity = scene_destroy_entity,
	.set_transform  = scene_set_transform,
	.set_name       = scene_set_name,
	.set_render_ref = scene_set_render_ref,
	.get_selected   = scene_get_selected,
	.set_selected   = scene_set_selected,
	.export_scene_bytes = scene_export_bytes,
	.ingest_scene_bytes = scene_ingest_bytes,
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
	g_edit  = subsystem_manager_get_api(mgr, "edit");
	/* Kept for a lazy "backend" lookup: it may register after this plugin,
	 * so note_scene_edit() resolves branching on first edit, not here. */
	g_mgr   = mgr;
	subsystem_manager_register(mgr, &scene_desc);
}
