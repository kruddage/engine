/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "renderer.h"
#include "renderer_null.h"
#include "fg.h"
#include "entity_api.h"
#include "asset_api.h"
#include "mesh.h"
#include "builtin_mesh_scripts.h"
#include "subsystem_manager.h"
#include "memory.h"
#include "script.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

void renderer_null_plugin_entry(struct subsystem_manager *mgr);
void fg_plugin_entry(struct subsystem_manager *mgr);
void scene_renderer_plugin_entry(struct subsystem_manager *mgr);

/* ------------------------------------------------------------------ */
/* A minimal fake asset catalog: the built-in meshes and the one scene */
/* shader (DSL, both stages), addressed by stable id (i + 1).          */
/* ------------------------------------------------------------------ */

static const char *const CAT_PATHS[] = {
	"builtin://mesh/cube",
	"builtin://mesh/sphere",
	"builtin://mesh/plane",
	"builtin://mesh/pyramid",
	"builtin://shader/scene",
	"material/red",
	"shader/alt",
	"material/blue",
};
#define CAT_COUNT ((uint32_t)(sizeof(CAT_PATHS) / sizeof(CAT_PATHS[0])))

/* Per-index asset type, addressed the same way as the id (id == index + 1). */
static const int32_t CAT_TYPES[CAT_COUNT] = {
	ASSET_TYPE_MESH,     /* id 1 cube    */
	ASSET_TYPE_MESH,     /* id 2 sphere  */
	ASSET_TYPE_MESH,     /* id 3 plane   */
	ASSET_TYPE_MESH,     /* id 4 pyramid */
	ASSET_TYPE_SHADER,   /* id 5 scene shader */
	ASSET_TYPE_MATERIAL, /* id 6 material/red (legacy 16-byte, no shader) */
	ASSET_TYPE_SHADER,   /* id 7 shader/alt   */
	ASSET_TYPE_MATERIAL, /* id 8 material/blue (v2: base_color + shader 7) */
};

/* ids 1-4 serve (mesh ...) source text — upload_mesh compiles it through the
 * real s7 image, exactly as the shipped asset plugin seeds these same four
 * built-ins (see asset_plugin.c). */
static const char *const CAT_MESH_SCRIPTS[4] = {
	CUBE_MESH_SCRIPT_SRC, SPHERE_MESH_SCRIPT_SRC,
	PLANE_MESH_SCRIPT_SRC, PYRAMID_MESH_SCRIPT_SRC,
};

/* The null backend records rather than compiles, so any DSL bytes suffice. */
static const char *const SHADER_SRC =
	"(shader scene (vertex (set position (vec4 0.0 0.0 0.0 1.0)))"
	" (fragment (set c (vec4 1.0 1.0 1.0 1.0))))";
static const char *const SHADER_ALT_SRC =
	"(shader alt (vertex (set position (vec4 0.0 0.0 0.0 1.0)))"
	" (fragment (set c (vec4 0.0 0.0 1.0 1.0))))";
/*
 * A v3 material's wire form: a leading shader-ref (asset id) then the shader's
 * std140 Material block — here one vec4 base_color. Red (id 6) names the scene
 * shader (id 5); blue (id 8) names shader/alt (id 7).
 */
static const struct {
	uint32_t shader_ref;
	float    base_color[4];
} MATERIAL_RED  = { 5u, { 1.0f, 0.0f, 0.0f, 1.0f } };
static const struct {
	uint32_t shader_ref;
	float    base_color[4];
} MATERIAL_BLUE = { 7u, { 0.0f, 0.0f, 1.0f, 1.0f } };

static uint32_t cat_count(void) { return CAT_COUNT; }

static int32_t cat_info(uint32_t i, struct asset_info *out)
{
	if (i >= CAT_COUNT || !out)
		return -1;
	memset(out, 0, sizeof(*out));
	out->path      = CAT_PATHS[i];
	out->id        = i + 1;
	out->type      = CAT_TYPES[i];
	out->state     = 1;
	out->read_only = 1;
	return 0;
}

/* Resolve a stable id (index + 1) to its snapshot; id 0 is "none". */
static int32_t cat_find(uint32_t id, struct asset_info *out)
{
	if (id == 0 || id > CAT_COUNT)
		return -1;
	return cat_info(id - 1, out);
}

static const void *cat_get_data(uint32_t id, uint32_t *out_size)
{
	if (id >= 1 && id <= 4) {
		const char *src = CAT_MESH_SCRIPTS[id - 1];

		if (out_size)
			*out_size = (uint32_t)strlen(src) + 1;
		return src;
	}
	if (id == 5) {
		if (out_size)
			*out_size = (uint32_t)strlen(SHADER_SRC) + 1;
		return SHADER_SRC;
	}
	if (id == 6) {
		if (out_size)
			*out_size = (uint32_t)sizeof(MATERIAL_RED);
		return &MATERIAL_RED;
	}
	if (id == 7) {
		if (out_size)
			*out_size = (uint32_t)strlen(SHADER_ALT_SRC) + 1;
		return SHADER_ALT_SRC;
	}
	if (id == 8) {
		if (out_size)
			*out_size = (uint32_t)sizeof(MATERIAL_BLUE);
		return &MATERIAL_BLUE;
	}
	return NULL;
}

static const struct asset_api FAKE_ASSET = {
	.count    = cat_count,
	.info     = cat_info,
	.find     = cat_find,
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

	/* Entity 0 carries a material (base_color red) and draws. Entity 1 has
	 * a mesh but no material — it must be skipped from drawing, though its
	 * mesh stays live (COMPONENT_RENDER) for picking/collision. */
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

/* Every draw binds the material UBO at slot 1 (a param-less material falls
 * back to a white tint; an entity with no material at all doesn't draw, so
 * it never reaches this bind). */
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

static uint32_t count_calls(enum gpu_call_type type)
{
	const struct gpu_call_record *log;
	uint32_t count, i, hits = 0;

	log = renderer_null_get_log(&count);
	for (i = 0; i < count; i++)
		if (log[i].type == type)
			hits++;
	return hits;
}

/*
 * Entity 0 carries the v2 material (id 8) whose shader-ref selects shader/alt;
 * entity 1 carries the v3 material (id 6) whose shader-ref names the scene
 * shader itself, so it reuses the pre-cached default pipeline. So a frame
 * binds two distinct pipelines, one switch between them.
 */
static void build_world_shaders(uint32_t cube_ref)
{
	memset(&g_world, 0, sizeof(g_world));
	g_world.count = 2;

	g_world.alive[0]        = 1;
	g_world.mask[0]         = COMPONENT_RENDER | COMPONENT_MATERIAL;
	g_world.render_ref[0]   = cube_ref;
	g_world.material_ref[0] = 8u;
	set_identity_xform(&g_world.world_xform[0], -1.0f, 0.0f, 0.0f);

	g_world.alive[1]        = 1;
	g_world.mask[1]         = COMPONENT_RENDER | COMPONENT_MATERIAL;
	g_world.render_ref[1]   = cube_ref;
	g_world.material_ref[1] = 6u;
	set_identity_xform(&g_world.world_xform[1], 1.0f, 0.0f, 0.0f);
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

	mem_init();
	script_init(); /* loads the embedded mesh_script.scm image */

	/* cube is catalog id 1. Two live entities reference it. */
	build_world(1);

	subsystem_manager_init(&mgr, static_table);
	renderer_null_plugin_entry(&mgr);   /* "renderer" */
	fg_plugin_entry(&mgr);              /* "frame_graph" (needs renderer) */
	scene_renderer_plugin_entry(&mgr);  /* resolves all, init uploads meshes */

	/* One forward pass: one draw per live entity that carries both a mesh
	 * and a material — entity 1's mesh is skipped since it has no material. */
	renderer_null_reset_log();
	subsystem_manager_tick(&mgr);
	assert(count_draws(&idx_count) == 1);
	assert(idx_count == 36);            /* cube: 36 indices */
	assert(count_material_binds() == 1); /* one per draw */

	/* Degrade safe: an empty world draws nothing and does not crash. */
	g_world.count = 0;
	renderer_null_reset_log();
	subsystem_manager_tick(&mgr);
	assert(count_draws(NULL) == 0);

	/*
	 * Per-material shader: a material that selects shader/alt gets its own
	 * pipeline, compiled once and off-frame (from the tick, not the pass).
	 * Every live entity still draws. (The recording backend returns a 0
	 * pipeline handle for every create, so a bind can't be attributed to a
	 * specific pipeline here — the create count is the observable signal.)
	 */
	build_world_shaders(1);
	renderer_null_reset_log();
	subsystem_manager_tick(&mgr);
	assert(count_calls(GPU_CALL_PIPELINE_CREATE) == 1); /* just shader/alt */
	assert(count_draws(NULL) == 2);

	/* The pipeline is cached by shader id: a later frame compiles nothing. */
	renderer_null_reset_log();
	subsystem_manager_tick(&mgr);
	assert(count_calls(GPU_CALL_PIPELINE_CREATE) == 0);
	assert(count_draws(NULL) == 2);

	subsystem_manager_shutdown(&mgr);
	mem_shutdown();

	printf("scene_renderer tests passed\n");
	return 0;
}
