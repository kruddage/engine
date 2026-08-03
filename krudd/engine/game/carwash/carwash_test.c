/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * carwash — the attract mode end to end against the real image, with no C
 * behind it. carwash.scm is one (project ...) form, so this test drives it the
 * way the engine does: project_host_eval registers the form on the real
 * launcher registry, and game_load runs the host's own load path — clear the
 * world, build the (scene ...) clause, run the (on-load ...) hook — over a
 * stand-in "scene" api bound to the world below.
 *
 * A show nobody clicks is a show with no input to check, so what is checked
 * instead is that it plays: the bay builds, the loop walks its six phases in
 * order and wraps, the three surface scalars follow the phase they belong to,
 * the paint at the catalog path is actually repacked as they move, the tools
 * leave their rack when their phase comes up and are back on it when it ends,
 * the car is carried down the lane and stands still while it is being worked
 * on, and the camera never ends up anywhere the view degenerates.
 *
 * No GPU and no browser: the entity-script driver ticks a bound script without
 * one, a material path that resolves binds without one, and the particle and
 * audio primitives the show reaches for are simply absent here — which the
 * rules guard for, so the loop runs dry and silent and every number below is
 * still the number the browser would produce.
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

#include "carwash_scm.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* One world instance reused across the checks; too big for the stack. */
static struct world w;

static struct subsystem_manager mgr;

/* The launcher slot carwash.scm took, from project_host_eval in main. */
static int32_t g_carwash;

/*
 * The real asset catalog, assembled from asset.h the way the asset plugin
 * assembles it for the subsystem manager. The show needs it for two things the
 * checks below are entirely about: binding the scripts that drive every moving
 * entity, and holding the paint material that the loop rewrites — which also
 * needs the seeded pbr shader here, since material-define! packs against the
 * Material block of the shader a source names.
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

/*
 * The stand-in "scene" subsystem: the three entity_api entries a project's host
 * uses, each binding this test's world exactly as entity_plugin.c binds the
 * live one.
 */
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

/*
 * Total entities the bay spawns: a camera, the floor and its pad, three gantry
 * pieces, three rack pieces, the car's nine parts, and the tools' eight. Its
 * fingerprint — it moves only when the (scene ...) clause of carwash.scm gains
 * or loses an entity.
 */
#define CARWASH_ENTITY_COUNT 26

/* The phases, in the order carwash.scm lists them. */
#define PHASE_ROLL_IN  0
#define PHASE_RINSE    1
#define PHASE_PRODUCT  2
#define PHASE_BUFF     3
#define PHASE_REVEAL   4
#define PHASE_ROLL_OUT 5

/*
 * Open the show the way the launcher does. game_load runs the project host's
 * own load callback, which clears the world, builds the (scene ...) clause into
 * it and then runs the (on-load ...) hook — so every check below starts from
 * the first frame of the loop, not from wherever the last check left it.
 */
static uint32_t open_carwash(void)
{
	game_load(g_carwash);
	return w.count;
}

/* Poll one of the show's integer readouts (see carwash.scm's poll hooks). */
static int32_t poll(const char *fn)
{
	return scene_script_call(&w, &catalog_api, fn, 0);
}

/*
 * Drive the show forward MS milliseconds. The clock the loop runs on is
 * normally turned by the scripts' own on-tick, which needs an entity-script
 * driver and therefore a bound scene; carwash-run! turns the same clock in the
 * same frame-sized steps for a caller that has neither, so a phase check does
 * not have to stand a whole bay up first.
 */
static int32_t run_ms(int32_t ms)
{
	return scene_script_call(&w, &catalog_api, "carwash-run!", ms);
}

/* #t when a live entity named NAME exists. */
static int has_named(const char *name)
{
	uint32_t e;

	for (e = 0; e < w.count; e++) {
		const char *n = world_entity_name(&w, e);

		if (n && strcmp(n, name) == 0)
			return 1;
	}
	return 0;
}

/* The entity id carrying NAME, asserted to exist. */
static uint32_t id_named(const char *name)
{
	uint32_t e;

	for (e = 0; e < w.count; e++) {
		const char *n = world_entity_name(&w, e);

		if (n && strcmp(n, name) == 0)
			return e;
	}
	assert(0 && "no such entity");
	return 0;
}

/* The stable catalog id at PATH, or 0 when nothing is there. */
static uint32_t asset_id_at(const char *path)
{
	struct asset_info info;
	uint32_t          i, n = asset_catalog_count();

	for (i = 0; i < n; i++) {
		if (asset_catalog_info(i, &info) == 0 && info.path
		    && strcmp(info.path, path) == 0)
			return info.id;
	}
	return 0;
}

/* The bay builds, with every part of it accounted for. */
static void test_scene_builds(void)
{
	assert(open_carwash() == CARWASH_ENTITY_COUNT);

	/* The camera the renderer reads its eye from, and the bay around it. */
	assert(has_named("Camera") && has_named("ground"));
	assert(has_named("gantry-beam") && has_named("rack-top"));

	/* The car: painted panels, glass, lamps and four wheels. */
	assert(has_named("car-body") && has_named("car-cabin"));
	assert(has_named("car-roof") && has_named("car-lamp-l"));
	assert(has_named("car-wheel-fl") && has_named("car-wheel-rr"));

	/* One entity per part of each of the three tools. */
	assert(has_named("tool-wand-lance") && has_named("tool-wand-tip"));
	assert(has_named("tool-pad-face") && has_named("tool-buffer-head"));
}

/*
 * Opening the show twice yields the same count — the build is deterministic,
 * and the host's load clears the world before it builds rather than stacking a
 * second bay on top of the first.
 */
static void test_rebuild_is_stable(void)
{
	uint32_t a, b;

	a = open_carwash();
	b = open_carwash();
	assert(a == b && a == CARWASH_ENTITY_COUNT);
}

/*
 * The loop: six phases in the authored order, wrapping back to the first. Each
 * step lands the clock in the middle of the next phase rather than on its edge,
 * so a phase that came out a frame long or short still reads as itself and the
 * check is about the ORDER, which is the thing that would break.
 */
static void test_phases_cycle(void)
{
	assert(open_carwash() == CARWASH_ENTITY_COUNT);

	assert(poll("carwash-phase") == PHASE_ROLL_IN);
	assert(run_ms(1200) == PHASE_ROLL_IN);      /* 1.2s of 2.4 */
	assert(run_ms(4200) == PHASE_RINSE);        /* 3.0s of 6.0 */
	assert(run_ms(6000) == PHASE_PRODUCT);
	assert(run_ms(6000) == PHASE_BUFF);
	assert(run_ms(4800) == PHASE_REVEAL);       /* 1.8s of 3.6 */
	assert(run_ms(3600) == PHASE_ROLL_OUT);     /* 1.2s of 2.4 */
	assert(run_ms(2400) == PHASE_ROLL_IN);      /* wrapped */
	assert(run_ms(4200) == PHASE_RINSE);        /* and round again */
}

/*
 * A tool is out for each of the three work phases and none is out for the other
 * three. This is the "switch tools every so often" the whole show is built
 * around, read back as the one number that decides which tool moves and what
 * the camera watches.
 */
static void test_tool_per_phase(void)
{
	assert(open_carwash() == CARWASH_ENTITY_COUNT);

	assert(poll("carwash-active-tool") == -1);  /* rolling in */
	run_ms(3600);
	assert(poll("carwash-phase") == PHASE_RINSE);
	assert(poll("carwash-active-tool") == 0);   /* the wand */
	run_ms(6000);
	assert(poll("carwash-active-tool") == 1);   /* the applicator */
	run_ms(6000);
	assert(poll("carwash-active-tool") == 2);   /* the buffer */
	run_ms(6000);
	assert(poll("carwash-phase") == PHASE_REVEAL);
	assert(poll("carwash-active-tool") == -1);  /* tools away */
}

/*
 * The surface follows the tool that is working it: the car arrives filthy, the
 * rinse takes the dirt off, the product goes on and then comes off under the
 * buff, and the shine that replaces it stays up until the loop wraps. These are
 * what the paint is packed from, so they are what "the car got clean" means
 * before any bytes are involved.
 */
static void test_surface_follows_the_phase(void)
{
	assert(open_carwash() == CARWASH_ENTITY_COUNT);

	/* Rolling in: filthy, no product, no shine. */
	assert(poll("carwash-clean-pct") == 0);
	assert(poll("carwash-product-pct") == 0);
	assert(poll("carwash-gloss-pct") == 0);

	/* Halfway through the rinse the dirt is halfway off. */
	run_ms(5400);
	assert(poll("carwash-phase") == PHASE_RINSE);
	assert(poll("carwash-clean-pct") > 30 && poll("carwash-clean-pct") < 70);

	/* By the product phase it is off, and the product is going on over it
	 * with no shine up yet — that is the buffer's to bring. */
	run_ms(6000);
	assert(poll("carwash-phase") == PHASE_PRODUCT);
	assert(poll("carwash-clean-pct") == 100);
	assert(poll("carwash-product-pct") > 30);
	assert(poll("carwash-gloss-pct") == 0);

	/* The buff takes the product back off and leaves the shine behind. */
	run_ms(10200);
	assert(poll("carwash-phase") == PHASE_REVEAL);
	assert(poll("carwash-product-pct") == 0);
	assert(poll("carwash-gloss-pct") == 100);

	/* And the wrap puts the next car back at filthy. */
	run_ms(6000);
	assert(poll("carwash-phase") == PHASE_ROLL_IN);
	assert(poll("carwash-clean-pct") == 0);
	assert(poll("carwash-gloss-pct") == 0);
}

/*
 * The car arrives from most of a lane away, stands still for the whole of the
 * wash, and leaves the other way. Standing still while it is worked on is the
 * part worth pinning: a tool pose is authored against a car at the origin, so a
 * car that drifted during a work phase would have every tool working the air
 * beside it.
 */
static void test_car_rolls_in_and_out(void)
{
	assert(open_carwash() == CARWASH_ENTITY_COUNT);

	assert(poll("carwash-car-x-mm") < -16000);   /* off down the lane */
	run_ms(3600);
	assert(poll("carwash-phase") == PHASE_RINSE);
	assert(poll("carwash-car-x-mm") == 0);       /* parked in the bay */
	run_ms(18000);
	assert(poll("carwash-phase") == PHASE_REVEAL);
	assert(poll("carwash-car-x-mm") == 0);       /* never moved */
	run_ms(4200);
	assert(poll("carwash-phase") == PHASE_ROLL_OUT);
	assert(poll("carwash-car-x-mm") > 10000);    /* and away */
}

/*
 * The camera's floors, checked over a whole lap. An eye that reaches the middle
 * of the bay is inside the car it is meant to be filming, and an eye directly
 * over the target has no horizontal distance left at all — the view degenerates
 * and the frame rolls. Both are what easing the eye in polar terms rather than
 * as a point is there to prevent, so both are asserted every frame of a lap
 * rather than at the ends of one.
 */
static void test_camera_stays_out_of_the_car(void)
{
	int i;

	assert(open_carwash() == CARWASH_ENTITY_COUNT);

	/* A lap is 26.4s; 1800 frames at 16ms is a little over one. */
	for (i = 0; i < 1800; i++) {
		run_ms(16);
		assert(poll("carwash-cam-r-mm") >= 6000);
		assert(poll("carwash-cam-h-mm") >= 2400);
	}
}

/*
 * The scripts this show brings with it, over the real catalog: carwash.scm
 * registered them when it was evaluated, the scene clause's (script ...)
 * resolves those paths, and the entity-script driver ticks them. The engine
 * seeded nothing for any of this — the whole chain exists because the project
 * brought it.
 */
static const char *SCRIPT_PATHS[] = {
	"project://script/carwash-camera",
	"project://script/carwash-car",
	"project://script/carwash-tool-0",
	"project://script/carwash-tool-1",
	"project://script/carwash-tool-2",
};

static void test_scripts_defined_and_bound(void)
{
	struct asset_info info;
	uint32_t          i, e;
	static const struct {
		const char *entity;
		const char *path;
	} BOUND[] = {
		{ "Camera",          "project://script/carwash-camera" },
		{ "car-body",        "project://script/carwash-car" },
		{ "car-wheel-fl",    "project://script/carwash-car" },
		{ "tool-wand-lance", "project://script/carwash-tool-0" },
		{ "tool-pad-face",   "project://script/carwash-tool-1" },
		{ "tool-buffer-head", "project://script/carwash-tool-2" },
	};

	assert(open_carwash() == CARWASH_ENTITY_COUNT);

	for (i = 0; i < sizeof(SCRIPT_PATHS) / sizeof(SCRIPT_PATHS[0]); i++)
		assert(asset_id_at(SCRIPT_PATHS[i]) != 0);

	for (i = 0; i < sizeof(BOUND) / sizeof(BOUND[0]); i++) {
		e = id_named(BOUND[i].entity);
		assert(w.mask[e] & COMPONENT_SCRIPT);
		assert(w.script_ref[e] != 0);
		assert(asset_catalog_find(w.script_ref[e], &info) == 0);
		assert(strcmp(info.path, BOUND[i].path) == 0);
	}

	/* The bay itself is not scripted — only what moves is. */
	assert(!(w.mask[id_named("ground")] & COMPONENT_SCRIPT));
	assert(!(w.mask[id_named("gantry-beam")] & COMPONENT_SCRIPT));
}

/* Run FRAMES frames of the real per-entity script tick, from T seconds. */
static float tick_frames(float t, int frames)
{
	int i;

	for (i = 0; i < frames; i++) {
		t += 0.016f;
		world_tick(&w, 16.0f);
		entity_script_tick(&w, &catalog_api, t);
	}
	return t;
}

/* Horizontal distance of an entity's render pose from the bay's centre. */
static float radius_xz(uint32_t e)
{
	const float *p = w.world_xform[e].position;

	return sqrtf(p[0] * p[0] + p[2] * p[2]);
}

/*
 * The show on screen, driven through the path the engine actually takes: the
 * entity-script driver ticking every bound script once a frame. This is the one
 * check that proves the clock is shared — the camera, the car and three tools
 * all call carwash-clock! with the same reading, and if it were not idempotent
 * on that reading five entities would advance the loop five times a frame and
 * everything below would be somewhere else entirely.
 */
static void test_the_show_runs(void)
{
	uint32_t cam  = 0, body = 0, lance = 0;
	float    t    = 0.0f;

	assert(open_carwash() == CARWASH_ENTITY_COUNT);
	cam   = id_named("Camera");
	body  = id_named("car-body");
	lance = id_named("tool-wand-lance");

	/* Frame one: the car is still off down the lane, where the roll-in
	 * puts it, and the wand is on its rack behind the bay. */
	t = tick_frames(t, 1);
	assert(w.world_xform[body].position[0] < -16.0f);
	assert(w.world_xform[lance].position[2] < -3.0f);

	/* Into the rinse: the car has arrived and the wand is off the rack and
	 * out at the flank, on the +Z side the sweep starts from. */
	t = tick_frames(t, 260);                       /* ~4.2s */
	assert(poll("carwash-phase") == PHASE_RINSE);
	assert(fabsf(w.world_xform[body].position[0]) < 0.01f);
	assert(w.world_xform[lance].position[2] > 0.5f);
	assert(radius_xz(lance) > 2.0f && radius_xz(lance) < 4.5f);

	/* The camera went with it: still outside the car, and moved off the
	 * opening hero shot rather than sitting where the scene authored it. */
	assert(radius_xz(cam) > 6.0f);
	assert(w.world_xform[cam].position[1] > 2.4f);
	assert(fabsf(w.world_xform[cam].position[2]) > 0.5f);

	/* The authored rest poses never moved: only the render pose animates. */
	assert(fabsf(w.local[body].position[0]) < 1e-4f);
	assert(fabsf(w.local[cam].position[0] - 12.4f) < 1e-4f);

	/* By the buff the wand is back on its rack and the buffer is out over
	 * the car, which is what a tool change looks like from the outside. */
	t = tick_frames(t, 760);                       /* ~12.2s */
	assert(poll("carwash-phase") == PHASE_BUFF);
	assert(w.world_xform[lance].position[2] < -3.0f);
	assert(w.world_xform[id_named("tool-buffer-head")].position[1] > 1.0f);
	assert(radius_xz(id_named("tool-buffer-head")) < 3.0f);
	(void)t;
}

/* The float stored at byte OFF of an asset's bytes. */
static float material_float(uint32_t id, uint32_t off)
{
	const uint8_t *b = (const uint8_t *)asset_catalog_get_data(id, NULL);
	float          v;

	assert(b);
	memcpy(&v, b + off, sizeof(v));
	return v;
}

/*
 * The paint's roughness, at the offset the pbr Material block declares
 * (base_color @0, metallic @16, roughness @20) past the 4-byte shader-ref
 * header. It is the field the whole wash is legible in: dust scatters, a
 * mirror does not.
 */
static float paint_roughness(void)
{
	uint32_t id = asset_id_at("project://material/carwash-paint");

	assert(id != 0);
	return material_float(id, 24);
}

/* Same, for the red channel of the base colour: dirt is brown, paint is red. */
static float paint_red(void)
{
	return material_float(asset_id_at("project://material/carwash-paint"), 4);
}

/*
 * The materials this show brings with it, and the one of them that moves.
 *
 * The static eight are checked for being there at all — they are the bay, and a
 * missing one is a panel drawing with the default pipeline. The paint is
 * checked for CHANGING: it is one asset at one path, repacked in place by the
 * loop, and the id surviving that is what lets the panels bound to it at load
 * time draw the new surface without rebinding. A repack that allocated a new id
 * would leave the car frozen at its dirtiest while the catalog filled up with
 * every surface it had ever been.
 */
static void test_paint_is_repacked_in_place(void)
{
	static const char *STATIC_PATHS[] = {
		"project://material/carwash-glass",
		"project://material/carwash-chrome",
		"project://material/carwash-rubber",
		"project://material/carwash-floor",
		"project://material/carwash-bay",
		"project://material/carwash-tool",
		"project://material/carwash-foam",
		"project://material/carwash-pad",
	};
	uint32_t i, paint, before, dirty_red;
	float    rough_dirty, rough_shiny;

	assert(open_carwash() == CARWASH_ENTITY_COUNT);

	for (i = 0; i < sizeof(STATIC_PATHS) / sizeof(STATIC_PATHS[0]); i++)
		assert(asset_id_at(STATIC_PATHS[i]) != 0);

	/* Filthy: a dull, scattering surface in the colour of road film. */
	paint       = asset_id_at("project://material/carwash-paint");
	before      = paint;
	rough_dirty = paint_roughness();
	dirty_red   = (uint32_t)(paint_red() * 1000.0f);
	assert(paint != 0);
	assert(rough_dirty > 0.8f);

	/* Buffed: the same asset, a mirror. */
	run_ms(2400 + 6000 + 6000 + 6000 + 1000);
	assert(poll("carwash-phase") == PHASE_REVEAL);
	rough_shiny = paint_roughness();
	assert(rough_shiny < 0.15f);
	assert(rough_shiny < rough_dirty);
	assert(asset_id_at("project://material/carwash-paint") == before);

	/* And redder, the dirt film having come off the paint under it. */
	assert((uint32_t)(paint_red() * 1000.0f) > dirty_red);

	/* Nothing named carwash is seeded on the show's behalf. */
	assert(asset_id_at("builtin://material/carwash-paint") == 0);
}

/*
 * The panels bind the paint the show rewrites, and the parts that are not
 * painted bind their own materials — so what changes over a wash is the car's
 * paint and not a wash over the whole frame.
 */
static void test_scene_binds_the_paint(void)
{
	static const struct {
		const char *entity;
		const char *path;
	} BOUND[] = {
		{ "car-body",     "project://material/carwash-paint" },
		{ "car-roof",     "project://material/carwash-paint" },
		{ "car-cabin",    "project://material/carwash-glass" },
		{ "car-lamp-l",   "project://material/carwash-chrome" },
		{ "car-wheel-fl", "project://material/carwash-rubber" },
		{ "ground",       "project://material/carwash-floor" },
	};
	uint32_t i, e;

	assert(open_carwash() == CARWASH_ENTITY_COUNT);

	for (i = 0; i < sizeof(BOUND) / sizeof(BOUND[0]); i++) {
		e = id_named(BOUND[i].entity);
		assert(w.material_ref[e] != 0);
		assert(w.material_ref[e] == asset_id_at(BOUND[i].path));
	}
}

/*
 * Reloading the show re-evaluates its source, which re-runs every script-define!
 * and material-define!. That must replace in place: the ids the live bay is
 * bound to have to survive, or a reload would leave the car pointing at a dead
 * material and every moving entity at a dead script.
 */
static void test_reload_keeps_bindings(void)
{
	uint32_t scripts[5], paint, i, e, cam;

	assert(open_carwash() == CARWASH_ENTITY_COUNT);
	for (i = 0; i < 5; i++) {
		scripts[i] = asset_id_at(SCRIPT_PATHS[i]);
		assert(scripts[i] != 0);
	}
	paint = asset_id_at("project://material/carwash-paint");
	e     = id_named("car-body");
	cam   = id_named("Camera");
	assert(w.material_ref[e] == paint);

	/* A reload: the whole project source evaluated again, which is how a
	 * project reloads. It keeps its launcher slot rather than stacking a
	 * second "Car Wash" beside the first. */
	assert(project_host_eval(CARWASH_SCM) == g_carwash);
	assert(game_count() == 1);

	for (i = 0; i < 5; i++)
		assert(asset_id_at(SCRIPT_PATHS[i]) == scripts[i]);
	assert(asset_id_at("project://material/carwash-paint") == paint);
	assert(w.material_ref[e] == paint);
	assert(w.script_ref[cam] == scripts[0]);
}

int main(void)
{
	mem_init();
	log_init();
	asset_init();         /* the real catalog the show's assets land in */
	script_init();        /* loads scene_script.scm (scene-build) */
	entity_script_init(); /* the entity-* primitives a script clause calls */
	/* Bind the catalog before the project source is evaluated: its
	 * top-level script-define! and material-define! calls are what put the
	 * show's scripts and its paint in it. */
	scene_script_bind_catalog(&catalog_api, &mut_api);
	scene_script_init();  /* registers the scene-* host primitives */
	world_reset(&w);

	/*
	 * The project host, brought up over a manager holding the stand-in
	 * "scene" api — the same two steps engine.c takes, minus the rest of
	 * the engine. Evaluating carwash.scm then loads the show's rules into
	 * the shared image, registers its assets, and puts "Car Wash" on the
	 * launcher.
	 */
	subsystem_manager_init(&mgr, t_subsystems);
	project_host_plugin_entry(&mgr);
	g_carwash = project_host_eval(CARWASH_SCM);
	assert(g_carwash >= 0);
	assert(game_count() == 1);
	assert(game_find("Car Wash") == g_carwash);

	test_scene_builds();
	test_rebuild_is_stable();
	test_phases_cycle();
	test_tool_per_phase();
	test_surface_follows_the_phase();
	test_car_rolls_in_and_out();
	test_camera_stays_out_of_the_car();
	test_scripts_defined_and_bound();
	test_the_show_runs();
	test_paint_is_repacked_in_place();
	test_scene_binds_the_paint();
	test_reload_keeps_bindings();

	printf("carwash_test: ok\n");
	return 0;
}
