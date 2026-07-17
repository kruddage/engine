/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * scene_script — the scene-building path end to end, minus the GPU. It boots the
 * real s7 image (which loads the embedded scene_script.scm), registers the
 * scene-* host primitives, and builds a (scene ...) form against a stand-in asset
 * catalog — then checks each spawned entity carries the transform, name, and
 * mesh/material/script bindings the form declared, and that the resolver turned
 * catalog paths into the right stable ids.
 */
#include "world.h"
#include "scene.h"
#include "asset_api.h"
#include "scene_script.h"

#include "script.h"
#include "log.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* One world instance reused across the checks; too big for the stack. */
static struct world w;

static int feq(float a, float b)
{
	float d = a - b;

	return (d < 0.0f ? -d : d) < 1e-4f;
}

/*
 * A stand-in catalog: six built-ins the scenes below reference, each at a stable
 * id the checks assert against. Only count/info are exercised — scene building
 * resolves paths to ids and never reads asset bytes.
 */
static const struct {
	const char *path;
	uint32_t    id;
	int32_t     type;
} g_catalog[] = {
	{ "builtin://mesh/plane",         10, ASSET_TYPE_MESH     },
	{ "builtin://mesh/torus",         11, ASSET_TYPE_MESH     },
	{ "builtin://mesh/box",           12, ASSET_TYPE_MESH     },
	{ "builtin://material/checker",   20, ASSET_TYPE_MATERIAL },
	{ "builtin://material/pbr-metal", 21, ASSET_TYPE_MATERIAL },
	{ "builtin://script/spinner",     30, ASSET_TYPE_SCRIPT   },
};

static uint32_t fake_count(void)
{
	return (uint32_t)(sizeof(g_catalog) / sizeof(g_catalog[0]));
}

static int32_t fake_info(uint32_t i, struct asset_info *out)
{
	if (!out || i >= fake_count())
		return -1;
	memset(out, 0, sizeof(*out));
	out->path = g_catalog[i].path;
	out->id   = g_catalog[i].id;
	out->type = g_catalog[i].type;
	return 0;
}

static const struct asset_api fake_asset = {
	.count = fake_count,
	.info  = fake_info,
};

/* The board + two marks scene, exercising every clause kind. */
static const char *SCENE_SRC =
	"(scene tic-tac-toe"
	"  (entity (name \"board\")"
	"          (mesh \"builtin://mesh/plane\")"
	"          (material \"builtin://material/checker\")"
	"          (at 0 0 0) (scale 3 3 3))"
	"  (entity (name \"o-a1\")"
	"          (mesh \"builtin://mesh/torus\")"
	"          (material \"builtin://material/pbr-metal\")"
	"          (at -1 0.15 -1) (scale 0.4 0.4 0.4))"
	"  (entity (name \"spin\")"
	"          (mesh \"builtin://mesh/box\")"
	"          (script \"builtin://script/spinner\")"
	"          (rotate 0 90 0)))";

/* A full build binds every declared asset, name, and transform. */
static void test_build_binds_everything(void)
{
	int32_t n;

	world_reset(&w);
	n = scene_script_build(&w, &fake_asset, SCENE_SRC);
	assert(n == 3);

	/* Entity 0: the board — mesh + material + name + position + scale. */
	assert(w.alive[0]);
	assert(w.mask[0] & COMPONENT_RENDER);
	assert(w.mask[0] & COMPONENT_MATERIAL);
	assert(w.mask[0] & COMPONENT_NAME);
	assert(w.render_ref[0] == 10);
	assert(w.material_ref[0] == 20);
	assert(strcmp(world_entity_name(&w, 0), "board") == 0);
	assert(feq(w.local[0].position[0], 0.0f));
	assert(feq(w.local[0].scale[0], 3.0f));
	assert(feq(w.local[0].scale[1], 3.0f));
	assert(feq(w.local[0].scale[2], 3.0f));

	/* Entity 1: an O — torus mesh, metal material, its own cell + scale. */
	assert(w.render_ref[1] == 11);
	assert(w.material_ref[1] == 21);
	assert(feq(w.local[1].position[0], -1.0f));
	assert(feq(w.local[1].position[1], 0.15f));
	assert(feq(w.local[1].position[2], -1.0f));
	assert(feq(w.local[1].scale[0], 0.4f));

	/* Entity 2: an X — box mesh + a bound script, no material. */
	assert(w.render_ref[2] == 12);
	assert(w.mask[2] & COMPONENT_SCRIPT);
	assert(w.script_ref[2] == 30);
	assert(!(w.mask[2] & COMPONENT_MATERIAL));
}

/*
 * (rotate 0 90 0) is a quarter turn about +Y: the authored quaternion is
 * (0, sin45, 0, cos45). The build writes the AUTHORED pose (local), leaving
 * world_xform for the tick to derive — so we read local here.
 */
static void test_rotate_builds_quaternion(void)
{
	float half = (float)(M_PI / 4.0);

	world_reset(&w);
	assert(scene_script_build(&w, &fake_asset, SCENE_SRC) == 3);
	assert(feq(w.local[2].rotation[0], 0.0f));
	assert(feq(w.local[2].rotation[1], sinf(half)));
	assert(feq(w.local[2].rotation[2], 0.0f));
	assert(feq(w.local[2].rotation[3], cosf(half)));
}

/* An unresolved path binds nothing (ref 0) and never faults the build. */
static void test_unknown_path_is_inert(void)
{
	int32_t n;

	world_reset(&w);
	n = scene_script_build(&w, &fake_asset,
			       "(scene s (entity (mesh \"builtin://mesh/nope\")))");
	assert(n == 1);
	assert(w.alive[0]);
	assert(w.render_ref[0] == 0);
	assert(!(w.mask[0] & COMPONENT_RENDER));
}

/* A non-scene form is rejected cleanly: nothing spawns, no crash. */
static void test_not_a_scene_form(void)
{
	world_reset(&w);
	assert(scene_script_build(&w, &fake_asset, "(mesh box)") == 0);
	assert(w.count == 0);
}

/* A per-entity fault is isolated: the good entities on either side still land. */
static void test_entity_fault_is_isolated(void)
{
	int32_t n;

	world_reset(&w);
	/* The middle entity's (name) clause has no argument, so reading its value
	 * throws — the per-entity catch drops just that one. It was already
	 * spawned (an empty entity 1) before the fault, so the surviving marks are
	 * entities 0 and 2. */
	n = scene_script_build(&w, &fake_asset,
			       "(scene s"
			       "  (entity (mesh \"builtin://mesh/box\"))"
			       "  (entity (name))"
			       "  (entity (mesh \"builtin://mesh/torus\")))");
	assert(n == 2);
	assert(w.render_ref[0] == 12);
	assert(!(w.mask[1] & COMPONENT_RENDER));
	assert(w.render_ref[2] == 11);
}

int main(void)
{
	log_init();
	script_init();       /* loads the embedded scene_script.scm image */
	scene_script_init(); /* registers the scene-* host primitives */

	test_build_binds_everything();
	test_rotate_builds_quaternion();
	test_unknown_path_is_inert();
	test_not_a_scene_form();
	test_entity_fault_is_isolated();

	printf("scene_script_test: ok\n");
	return 0;
}
