/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "renderer.h"
#include "renderer_null.h"
#include "fg.h"
#include "entity_api.h"
#include "asset_api.h"
#include "mesh.h"
#include "primitives.h"
#include "subsystem_manager.h"
#include "memory.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

void renderer_null_plugin_entry(struct subsystem_manager *mgr);
void fg_plugin_entry(struct subsystem_manager *mgr);
void scene_renderer_plugin_entry(struct subsystem_manager *mgr);

/* ------------------------------------------------------------------ */
/* A minimal fake asset catalog: the built-in primitive blobs and the  */
/* one scene shader (DSL, both stages), addressed by stable id (i + 1). */
/* ------------------------------------------------------------------ */

/* Route through modules/memory (raw malloc is banned outside it). */
static const struct memory_api TEST_MEM = {
	mem_alloc, mem_alloc_zero, mem_free,
	mem_pool_create, mem_pool_alloc, mem_pool_free, mem_pool_destroy,
};

static const char *const CAT_PATHS[] = {
	"builtin://cube",
	"builtin://sphere",
	"builtin://plane",
	"builtin://pyramid",
	"builtin://shader/scene",
	"material/red",
};
#define CAT_COUNT ((uint32_t)(sizeof(CAT_PATHS) / sizeof(CAT_PATHS[0])))

static void    *g_blob[4];       /* cube, sphere, plane, pyramid */
static uint32_t g_blob_size[4];
/* The null backend records rather than compiles, so any DSL bytes suffice. */
static const char *const SHADER_SRC =
	"(shader scene (vertex (set position (vec4 0.0 0.0 0.0 1.0)))"
	" (fragment (set c (vec4 1.0 1.0 1.0 1.0))))";
/* A material asset is just its base_color RGBA, raw floats — id 6. */
static const float MATERIAL_RED[4] = { 1.0f, 0.0f, 0.0f, 1.0f };

static uint32_t cat_count(void) { return CAT_COUNT; }

static int32_t cat_info(uint32_t i, struct asset_info *out)
{
	if (i >= CAT_COUNT || !out)
		return -1;
	memset(out, 0, sizeof(*out));
	out->path      = CAT_PATHS[i];
	out->id        = i + 1;
	out->type      = (i == 5) ? ASSET_TYPE_MATERIAL : ASSET_TYPE_MESH;
	out->state     = 1;
	out->read_only = 1;
	return 0;
}

static const void *cat_get_data(uint32_t id, uint32_t *out_size)
{
	if (id >= 1 && id <= 4) {
		if (out_size)
			*out_size = g_blob_size[id - 1];
		return g_blob[id - 1];
	}
	if (id == 5) {
		if (out_size)
			*out_size = (uint32_t)strlen(SHADER_SRC) + 1;
		return SHADER_SRC;
	}
	if (id == 6) {
		if (out_size)
			*out_size = (uint32_t)sizeof(MATERIAL_RED);
		return MATERIAL_RED;
	}
	return NULL;
}

static const struct asset_api FAKE_ASSET = {
	.count    = cat_count,
	.info     = cat_info,
	.get_data = cat_get_data,
};

/* ------------------------------------------------------------------ */
/* A fake "scene" exposing a hand-built world.                         */
/* ------------------------------------------------------------------ */

static struct world       g_world;
static const struct world *fake_get_world(void) { return &g_world; }

static const struct entity_api FAKE_SCENE = {
	.get_world = fake_get_world,
};

static void set_identity_xform(struct transform *t, float x, float y, float z)
{
	memset(t, 0, sizeof(*t));
	t->position[0] = x;
	t->position[1] = y;
	t->position[2] = z;
	t->rotation[3] = 1.0f;
	t->scale[0] = t->scale[1] = t->scale[2] = 1.0f;
}

/* Two live cube entities, one tombstoned render entity, one non-render. */
static void build_world(uint32_t cube_ref)
{
	memset(&g_world, 0, sizeof(g_world));
	g_world.count = 4;

	/* Entity 0 carries a material (base_color red); entity 1 has none and
	 * must fall back to the renderer's default (opaque white) tint. */
	g_world.alive[0]        = 1;
	g_world.mask[0]         = COMPONENT_RENDER | COMPONENT_MATERIAL;
	g_world.render_ref[0]   = cube_ref;
	g_world.material_ref[0] = 6u;
	set_identity_xform(&g_world.world_xform[0], -1.0f, 0.0f, 0.0f);

	g_world.alive[1]      = 1;
	g_world.mask[1]       = COMPONENT_RENDER;
	g_world.render_ref[1] = cube_ref;
	set_identity_xform(&g_world.world_xform[1], 1.0f, 0.0f, 0.0f);

	g_world.alive[2]      = 0; /* tombstoned — must be skipped */
	g_world.mask[2]       = COMPONENT_RENDER;
	g_world.render_ref[2] = cube_ref;
	set_identity_xform(&g_world.world_xform[2], 0.0f, 0.0f, 0.0f);

	g_world.alive[3]      = 1;
	g_world.mask[3]       = 0; /* no COMPONENT_RENDER — must be skipped */
	set_identity_xform(&g_world.world_xform[3], 0.0f, 2.0f, 0.0f);
}

static uint32_t count_draws(uint32_t *out_index_count)
{
	const struct gpu_call_record *log;
	uint32_t count, i, draws = 0;

	log = renderer_null_get_log(&count);
	for (i = 0; i < count; i++) {
		if (log[i].type != GPU_CALL_CMD_DRAW_INDEXED)
			continue;
		draws++;
		if (out_index_count)
			*out_index_count = log[i].args.cmd_draw_indexed.index_count;
	}
	return draws;
}

/* Every draw binds the material UBO at slot 1, materialed or not (the
 * renderer falls back to a white tint when an entity has no material_ref). */
static uint32_t count_material_binds(void)
{
	const struct gpu_call_record *log;
	uint32_t count, i, binds = 0;

	log = renderer_null_get_log(&count);
	for (i = 0; i < count; i++) {
		if (log[i].type == GPU_CALL_CMD_BIND_UNIFORM_BUFFER &&
		    log[i].args.cmd_bind_uniform_buffer.slot == 1 &&
		    log[i].args.cmd_bind_uniform_buffer.size == 4 * sizeof(float))
			binds++;
	}
	return binds;
}

int main(void)
{
	static const struct subsystem static_table[] = {
		{ .name = "asset", .api = &FAKE_ASSET },
		{ .name = "scene", .api = &FAKE_SCENE },
		{ NULL },
	};
	struct subsystem_manager mgr;
	uint32_t idx_count = 0;
	int      k;

	mem_init();

	/* Real primitive geometry for the fake catalog to serve. */
	for (k = 0; k < 4; k++) {
		g_blob[k] = primitive_generate((enum primitive_kind)k,
					       &TEST_MEM, &g_blob_size[k]);
		assert(g_blob[k] != NULL);
	}

	/* cube is catalog id 1. Two live entities reference it. */
	build_world(1);

	subsystem_manager_init(&mgr, static_table);
	renderer_null_plugin_entry(&mgr);   /* "renderer" */
	fg_plugin_entry(&mgr);              /* "frame_graph" (needs renderer) */
	scene_renderer_plugin_entry(&mgr);  /* resolves all, init uploads meshes */

	/* One forward pass: exactly one draw per live COMPONENT_RENDER entity. */
	renderer_null_reset_log();
	subsystem_manager_tick(&mgr);
	assert(count_draws(&idx_count) == 2);
	assert(idx_count == 36);            /* cube: 36 indices */
	assert(count_material_binds() == 2); /* one per draw, materialed or not */

	/* Degrade safe: an empty world draws nothing and does not crash. */
	g_world.count = 0;
	renderer_null_reset_log();
	subsystem_manager_tick(&mgr);
	assert(count_draws(NULL) == 0);

	subsystem_manager_shutdown(&mgr);
	for (k = 0; k < 4; k++)
		mem_free(g_blob[k]);
	mem_shutdown();

	printf("scene_renderer tests passed\n");
	return 0;
}
