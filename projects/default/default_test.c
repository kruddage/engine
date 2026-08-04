/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * default — the scene the page opens on, against the real image and the real
 * catalog.
 *
 * default.scm is one (project ...) form, so this drives it the way the engine
 * does: project_host_eval registers the form on the real launcher registry, and
 * game_load runs the host's own load path — clear the world, build the
 * (scene ...) clause — over a stand-in "scene" api bound to the world below.
 *
 * What it checks, and why these things and not others: this project defines no
 * asset of its own, so every mesh, material and script in it is a BUILT-IN
 * named by catalog path. A path that stops resolving — a renamed built-in, a
 * typo in the move out of C — binds nothing and says nothing: the entity still
 * spawns, still has a transform, and simply never draws or never moves. That
 * silence is the failure mode this file exists for, so the checks below are
 * mostly "the binding is real", read off the world as an asset id and, for the
 * three animated props and the camera, as movement after a tick.
 *
 * The layout is checked too, against the numbers seed_demo_scene spawned these
 * same five props at, since the claim #1034 makes is that the scene came across
 * unchanged rather than that a scene arrived.
 *
 * No GPU and no browser: a mesh resolves to an id here, not to geometry.
 */
#include <entity/world.h>
#include <entity/scene_script.h>
#include "entity_script.h"

#include "asset.h"
#include <abi/asset_api.h>
#include <abi/entity_api.h>

#include <core/script.h>
#include <core/subsystem_manager.h>
#include <host/game.h>
#include <log/log.h>
#include <memory/memory.h>
#include <project/project_host.h>

#include "staged_project_scm.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* One world instance reused across the checks; too big for the stack. */
static struct world w;

static struct subsystem_manager mgr;

/* The launcher slot default.scm took, from project_host_eval in main. */
static int32_t g_default;

/*
 * Total entities the scene spawns: the camera, the floor, and the four props.
 * Its fingerprint — it moves only when the (scene ...) clause of default.scm
 * gains or loses an entity.
 */
#define DEFAULT_ENTITY_COUNT 6

/*
 * The real asset catalog, assembled from asset.h the way the asset plugin
 * assembles it for the subsystem manager. Everything this project names lives
 * in it, which is why it is here at all.
 */
static const struct asset_api catalog_api = {
	.count    = asset_catalog_count,
	.info     = asset_catalog_info,
	.describe = asset_catalog_describe,
	.find     = asset_catalog_find,
	.get_data = asset_catalog_get_data,
};

static const struct asset_mut_api mut_api = {
	.create   = asset_mut_create,
	.set_data = asset_mut_set_data,
	.destroy  = asset_mut_destroy,
	.set_decl = asset_mut_set_decl,
	.inject   = asset_mut_inject,
};

static int32_t t_build_scene_scm(const char *src)
{
	return scene_script_build(&w, &catalog_api, src);
}

static int32_t t_dispatch_scm(const char *fn, int32_t arg)
{
	return scene_script_call(&w, &catalog_api, fn, arg);
}

static void t_clear_world(void)
{
	world_reset(&w);
}

static const struct entity_api t_scene_api = {
	.build_scene_scm = t_build_scene_scm,
	.dispatch_scm    = t_dispatch_scm,
	.clear_world     = t_clear_world,
};

static const struct subsystem t_subsystems[] = {
	{ .name = "scene", .api = &t_scene_api },
	{ NULL }
};

/* A thousandth of a unit, the tolerance every position check here runs at. */
#define EPS 0.001f

static int near(float a, float b)
{
	return fabsf(a - b) < EPS;
}

static uint32_t open_default(void)
{
	game_load(g_default);
	return w.count;
}

/* The entity named NAME. Asserts rather than returns a sentinel: every name
 * asked for below is one the scene is required to have. */
static uint32_t id_named(const char *name)
{
	uint32_t e;

	for (e = 0; e < w.count; e++) {
		const char *n = world_entity_name(&w, e);

		if (n && strcmp(n, name) == 0)
			return e;
	}
	assert(0 && "entity not in the built scene");
	return 0;
}

static const float *pos_of(const char *name)
{
	return w.local[id_named(name)].position;
}

/*
 * The catalog id PATH resolves to, asserted non-zero. This is the check the
 * whole file is built around: 0 is what an unknown path answers, and an entity
 * bound to 0 is the silent failure described up top.
 */
static uint32_t asset_id(const char *path)
{
	struct asset_info info;
	uint32_t          i, n = asset_catalog_count();

	for (i = 0; i < n; i++) {
		if (asset_catalog_info(i, &info) == 0 && info.path
		    && strcmp(info.path, path) == 0) {
			assert(info.id != 0);
			return info.id;
		}
	}
	assert(0 && "built-in asset path is not in the catalog");
	return 0;
}

/*
 * Entity NAME wears the mesh and material at those catalog paths — not merely
 * "has some ref", but the id the catalog hands back for the path default.scm
 * names. Both components are asserted present first, since a ref column is only
 * meaningful behind its bit.
 */
static void assert_wears(const char *name, const char *mesh,
			 const char *material)
{
	uint32_t e = id_named(name);

	assert(w.mask[e] & COMPONENT_RENDER);
	assert(w.render_ref[e] == asset_id(mesh));
	assert(w.mask[e] & COMPONENT_MATERIAL);
	assert(w.material_ref[e] == asset_id(material));
}

/* Entity NAME is bound to the behaviour script at that catalog path. */
static void assert_scripted(const char *name, const char *script)
{
	uint32_t e = id_named(name);

	assert(w.mask[e] & COMPONENT_SCRIPT);
	assert(w.script_ref[e] == asset_id(script));
}

/*
 * Run the scene forward to T seconds from a fresh load, in 16 ms frames, and
 * leave the world sitting there. world_tick THEN the scripts, which is the
 * engine's own order: world_tick composes each entity's world transform from
 * its authored local one, and a script's entity-set-position! then overwrites
 * that composed result for the frame. So an animated entity is read out of
 * world_xform and an authored one out of local — two different questions.
 */
static void run_to(float t)
{
	float now = 0.0f;

	assert(open_default() == DEFAULT_ENTITY_COUNT);
	while (now < t) {
		now += 0.016f;
		if (now > t)
			now = t;
		world_tick(&w, 16.0f);
		entity_script_tick(&w, &catalog_api, now);
	}
}

static const float *animated_pos(const char *name)
{
	return w.world_xform[id_named(name)].position;
}

/* The scene builds, whole, and builds the same twice — the host clears before
 * it builds rather than stacking a second copy on the first. */
static void test_scene_builds(void)
{
	assert(open_default() == DEFAULT_ENTITY_COUNT);
	assert(open_default() == DEFAULT_ENTITY_COUNT);

	id_named("Camera");
	id_named("Floor");
	id_named("Box");
	id_named("Sphere");
	id_named("Pyramid");
	id_named("Rook");
}

/*
 * The props stand where seed_demo_scene stood them: the floor a 6-unit square
 * half a unit down, and the four shapes spread around the origin. These are the
 * numbers the C table held, so this is the check that the scene came across
 * rather than that a scene exists.
 */
static void test_props_stand_where_the_seed_stood_them(void)
{
	const float *floor_s;

	assert(open_default() == DEFAULT_ENTITY_COUNT);

	assert(near(pos_of("Floor")[0], 0.0f));
	assert(near(pos_of("Floor")[1], -0.5f));
	assert(near(pos_of("Floor")[2], 0.0f));
	floor_s = w.local[id_named("Floor")].scale;
	assert(near(floor_s[0], 6.0f) && near(floor_s[2], 6.0f));

	assert(near(pos_of("Box")[0], -1.5f));
	assert(near(pos_of("Sphere")[2], -1.0f));
	assert(near(pos_of("Pyramid")[0], 1.5f));
	assert(near(pos_of("Pyramid")[2], 0.5f));
	assert(near(pos_of("Rook")[2], 1.4f));

	/* Everything but the floor is left at the DSL's default unit scale,
	 * which is what the seed's table set by hand. */
	assert(near(w.local[id_named("Box")].scale[0], 1.0f));
	assert(near(w.local[id_named("Rook")].scale[1], 1.0f));
}

/*
 * Every prop wears a real, resolvable built-in. The mix is the point of the
 * scene — both scene shaders on screen at once, and all three mesh shape
 * engines — so it is asserted per entity rather than as a count of bound
 * things.
 */
static void test_every_binding_resolves(void)
{
	assert(open_default() == DEFAULT_ENTITY_COUNT);

	assert_wears("Floor", "builtin://mesh/plane",
		     "builtin://material/checker");
	assert_wears("Box", "builtin://mesh/box",
		     "builtin://material/pbr-plastic");
	assert_wears("Sphere", "builtin://mesh/sphere",
		     "builtin://material/pbr-metal");
	assert_wears("Pyramid", "builtin://mesh/pyramid",
		     "builtin://material/checker");
	assert_wears("Rook", "builtin://mesh/sdf-rook",
		     "builtin://material/pbr-metal");

	assert_scripted("Box", "builtin://script/spinner");
	assert_scripted("Sphere", "builtin://script/bounce");
	assert_scripted("Pyramid", "builtin://script/wobble");
	assert_scripted("Rook", "builtin://script/spinner");

	/* The camera is a position with a script on it: no mesh, no material,
	 * and that is deliberate rather than an omission. */
	assert_scripted("Camera", "builtin://script/orbit-camera");
	assert(!(w.mask[id_named("Camera")] & COMPONENT_RENDER));
}

/*
 * The scene is alive: the bound scripts really tick. Checked as MOVEMENT
 * between two moments rather than against a pose, so nothing here restates a
 * formula from the built-in scripts and the checks cannot pass by agreeing with
 * a copy of them.
 *
 * The floor is the control. It carries no script, so it must sit exactly where
 * it was authored no matter how far time is run — which is what makes "these
 * four moved" mean the scripts moved them and not that the world drifts.
 */
static void test_the_scene_animates(void)
{
	float box_rot_y, sphere_y, pyramid_rot_x, cam_x, cam_z;

	run_to(0.5f);
	sphere_y  = animated_pos("Sphere")[1];
	pyramid_rot_x = w.world_xform[id_named("Pyramid")].rotation[0];
	box_rot_y  = w.world_xform[id_named("Box")].rotation[1];
	cam_x     = animated_pos("Camera")[0];
	cam_z     = animated_pos("Camera")[2];

	run_to(1.5f);

	/* The spinner turns the box about Y, the bounce lifts the sphere, the
	 * wobble tilts the pyramid, and the orbit-camera carries the eye around
	 * the scene — four scripts, four different kinds of motion. */
	assert(!near(w.world_xform[id_named("Box")].rotation[1], box_rot_y));
	assert(!near(animated_pos("Sphere")[1], sphere_y));
	assert(!near(w.world_xform[id_named("Pyramid")].rotation[0], pyramid_rot_x));
	assert(!near(animated_pos("Camera")[0], cam_x) ||
	       !near(animated_pos("Camera")[2], cam_z));

	/* The sphere hops ABOVE its rest position and never sinks below it —
	 * the bounce is a rectified sine off the authored Y, so a floor-through
	 * would mean the script lost its base position. */
	assert(animated_pos("Sphere")[1] >= pos_of("Sphere")[1] - EPS);

	/* The control. */
	assert(near(animated_pos("Floor")[1], -0.5f));
	assert(!(w.mask[id_named("Floor")] & COMPONENT_SCRIPT));
}

/*
 * The camera orbits rather than drifting off: the built-in script parks it on a
 * circle of a fixed radius about the origin, so the distance out is the
 * invariant and the angle is what moves. Read at several moments across a full
 * turn, which is also what proves it is going round rather than out.
 */
static void test_the_camera_orbits(void)
{
	float first = 0.0f;
	int   i;

	for (i = 0; i < 8; i++) {
		const float *p;
		float        r;

		run_to(1.0f + (float)i);
		p = animated_pos("Camera");
		r = sqrtf(p[0] * p[0] + p[2] * p[2]);
		if (i == 0)
			first = r;
		else
			assert(near(r, first));
		assert(r > 0.0f);
	}
}

int main(void)
{
	mem_init();
	log_init();
	script_init();        /* the image the catalog's seeding bakes through */
	asset_init();         /* the real catalog, and every built-in in it */
	entity_script_init(); /* the entity-* primitives a script clause calls */
	scene_script_bind_catalog(&catalog_api, &mut_api);
	scene_script_init();
	world_reset(&w);

	subsystem_manager_init(&mgr, t_subsystems);
	project_host_plugin_entry(&mgr);
	g_default = project_host_eval(STAGED_PROJECT_SCM);
	assert(g_default >= 0);
	assert(game_count() == 1);

	/*
	 * The name the launcher holds it under, and the name a ?game= resolves
	 * against — case-insensitively, which is what lets the page's own
	 * button (labelled from the filename, "default") open the project the
	 * source registered as "Default".
	 */
	assert(game_find("Default") == g_default);
	assert(game_find("default") == g_default);

	test_scene_builds();
	test_props_stand_where_the_seed_stood_them();
	test_every_binding_resolves();
	test_the_scene_animates();
	test_the_camera_orbits();

	printf("default_test: ok\n");
	return 0;
}
