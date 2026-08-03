/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ducks — the booth end to end against the real image.
 *
 * ducks.scm is one (project ...) form, so this drives it the way the engine
 * does: project_host_eval registers the form on the real launcher registry,
 * game_load runs the host's own load path, and the three entity scripts the
 * game registers are ticked by entity_script_tick against the real asset
 * catalog — which is the only way to check the conveyor at all, since the
 * conveyor IS a script and a script that never binds never moves anything.
 *
 * What it checks: the booth builds; the rank starts evenly spaced at its
 * authored positions; the conveyor carries every duck at the same speed and
 * wraps one off the right end back onto the left WITHOUT losing the overshoot
 * or breaking the spacing; the gun's aim sweeps the full width of the lane and
 * never past it; and the trigger scores a duck it is over and does not score
 * one it is not.
 *
 * The hit checks deliberately derive their moment from the WORLD rather than
 * recomputing the aim in C: the test ticks time forward, reads the crosshair
 * and the ducks back off the built scene, and looks for a frame where the gap
 * is inside the hit radius and one where it is well outside. Nothing here
 * restates a formula from ducks.scm, so the checks cannot agree with it by
 * being a copy of it.
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

#include "ducks_scm.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* One world instance reused across the checks; too big for the stack. */
static struct world w;

static struct subsystem_manager mgr;

/* The launcher slot ducks.scm took, from project_host_eval in main. */
static int32_t g_ducks;

/* The booth, in metres — the numbers ducks.scm is written in. */
#define LANE_MIN   (-3.5f)
#define LANE_MAX   3.5f
#define LANE_SPAN  7.0f
#define DUCK_COUNT 5
#define SPACING    (LANE_SPAN / DUCK_COUNT)
#define SPEED      0.9f
#define HIT_RADIUS 0.22f

/*
 * Total entities the booth spawns: a camera, the ground, a backboard, a
 * counter, the conveyor, five ducks, the gun's post and barrel, and the
 * crosshair. Its fingerprint — it moves only when the (scene ...) clause of
 * ducks.scm gains or loses an entity.
 */
#define DUCKS_ENTITY_COUNT 13

/* The real asset catalog, assembled from asset.h the way the asset plugin
 * assembles it for the subsystem manager. The scripts and materials this game
 * registers only bind if the dispatch can see it. */
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

/* A millimetre, the tolerance every position check here runs at. */
#define MM 0.001f

static int near(float a, float b)
{
	return fabsf(a - b) < MM;
}

static uint32_t open_ducks(void)
{
	game_load(g_ducks);
	return w.count;
}

static uint32_t id_named(const char *name)
{
	uint32_t e;

	for (e = 0; e < w.count; e++) {
		const char *n = world_entity_name(&w, e);

		if (n && strcmp(n, name) == 0)
			return e;
	}
	assert(0 && "entity not in the built booth");
	return 0;
}

static uint32_t duck_id(int i)
{
	char name[16];

	snprintf(name, sizeof(name), "duck-%d", i);
	return id_named(name);
}

/*
 * Run the booth forward to T seconds from a fresh load, in 16 ms frames, and
 * leave the world sitting there. The scripts read the frame time, so time is
 * what has to be advanced.
 *
 * world_tick THEN the scripts, which is the engine's own order and the one that
 * matters: world_tick composes each entity's world transform from its authored
 * local one, and entity-set-position! then overwrites that composed result for
 * this frame. Ticking the scripts first would have every write recomposed away
 * before anything read it. So the animated entities here are read out of
 * world_xform and the authored rank out of local — the two are different
 * questions, not two spellings of one.
 */
static void run_to(float t)
{
	float now = 0.0f;

	assert(open_ducks() == DUCKS_ENTITY_COUNT);
	while (now < t) {
		now += 0.016f;
		if (now > t)
			now = t;
		world_tick(&w, 16.0f);
		entity_script_tick(&w, &catalog_api, now);
	}
}

static float duck_x(int i)
{
	return w.world_xform[duck_id(i)].position[0];
}

static float crosshair_x(void)
{
	return w.world_xform[id_named("crosshair")].position[0];
}

/* The booth builds, whole, and builds the same twice. */
static void test_booth_builds(void)
{
	assert(open_ducks() == DUCKS_ENTITY_COUNT);
	assert(open_ducks() == DUCKS_ENTITY_COUNT);

	assert(w.count == DUCKS_ENTITY_COUNT);
	id_named("Camera");
	id_named("conveyor");
	id_named("gun-post");
	id_named("gun-barrel");
	id_named("crosshair");
}

/*
 * The rank as authored is the rank at rest: duck I starts one spacing along
 * from duck I-1, beginning at the left end of the lane. This is what makes the
 * booth look right in the instant before a single frame has ticked.
 */
static void test_rank_starts_evenly_spaced(void)
{
	int i;

	assert(open_ducks() == DUCKS_ENTITY_COUNT);

	for (i = 0; i < DUCK_COUNT; i++)
		assert(near(w.local[duck_id(i)].position[0],
			    LANE_MIN + i * SPACING));
}

/*
 * The conveyor carries the whole rank at one speed. Checked as a DISPLACEMENT
 * over a known interval rather than against an absolute position, so what is
 * being tested is that the ducks move at the speed the game says and together —
 * not that a particular duck is at a particular number.
 */
static void test_conveyor_carries_the_rank(void)
{
	float before[DUCK_COUNT];
	int   i;

	run_to(1.0f);
	for (i = 0; i < DUCK_COUNT; i++)
		before[i] = duck_x(i);
	/* The conveyor really is bound and running: a duck script that never
	 * bound would leave every duck sitting on its authored spot, which is
	 * the failure this whole check would otherwise pass straight over. */
	assert(w.mask[duck_id(0)] & COMPONENT_SCRIPT);

	/* One second on: every duck has advanced by the same one second's
	 * travel, and none of these has reached the end to wrap. */
	assert(near(before[0], LANE_MIN + SPEED));

	run_to(2.0f);
	for (i = 0; i < DUCK_COUNT; i++) {
		float moved = duck_x(i) - before[i];

		/* A duck that wrapped in this second moved back by a lane's
		 * width instead; either way it travelled the same distance. */
		if (moved < 0.0f)
			moved += LANE_SPAN;
		assert(near(moved, SPEED));
	}
}

/*
 * THE WRAP. A duck running off the right end reappears at the left carrying its
 * overshoot — the check that it does not lose a frame's travel to the fold, and
 * that the rank stays evenly spaced across it rather than bunching up once a
 * lap.
 *
 * Every duck is somewhere on the lane at every moment, and the gaps between
 * consecutive ducks (measured the short way round the loop) all stay one
 * spacing. That is the seamlessness claim in ducks.scm stated as an invariant
 * and checked across a full lap plus a bit, so every duck wraps at least once.
 */
static void test_conveyor_wraps_seamlessly(void)
{
	float t;

	for (t = 0.0f; t < 9.0f; t += 0.25f) {
		int i;

		run_to(t);
		for (i = 0; i < DUCK_COUNT; i++) {
			float x = duck_x(i);
			float gap;

			/* Always on the lane, never off the end of it. */
			assert(x >= LANE_MIN - MM && x <= LANE_MAX + MM);

			/* One spacing behind the next duck, the short way
			 * round. */
			gap = duck_x((i + 1) % DUCK_COUNT) - x;
			if (gap < 0.0f)
				gap += LANE_SPAN;
			assert(near(gap, SPACING));
		}
	}
}

/*
 * The aim sweeps the lane and stops there. The sweep is built out of the pair
 * of inverses ducks.scm defines — a yaw for a point on the lane, and the point
 * on the lane for a yaw — so driving it to its extreme and finding the lane's
 * exact end is the two of them agreeing. If either drifted, the crosshair would
 * fall short of the end duck or sail off past it.
 *
 * The extreme is a quarter turn of the sweep's phase: rate * t = pi/2.
 */
static void test_aim_sweeps_the_whole_lane(void)
{
	const float sweep_rate = 0.6f;   /* ducks-sweep-rate, radians/second */
	float       t;
	float       lo = LANE_MAX;
	float       hi = LANE_MIN;

	/* Dead centre at rest: sin(0) is 0, so the gun points straight down
	 * the booth. */
	run_to(0.0f);
	assert(near(crosshair_x(), 0.0f));

	/* A quarter of the sweep's phase later it is exactly on the right-hand
	 * end of the lane — not near it. */
	run_to((float)(M_PI / 2.0) / sweep_rate);
	assert(fabsf(crosshair_x() - LANE_MAX) < 0.01f);

	/* And over a full phase it covers the lane both ways and never leaves
	 * it. */
	for (t = 0.0f; t < (float)(2.0 * M_PI) / sweep_rate; t += 0.1f) {
		float x;

		run_to(t);
		x = crosshair_x();
		assert(x >= LANE_MIN - 0.01f && x <= LANE_MAX + 0.01f);
		if (x < lo)
			lo = x;
		if (x > hi)
			hi = x;
	}
	assert(hi > LANE_MAX - 0.05f);
	assert(lo < LANE_MIN + 0.05f);
}

/* The smallest gap between the crosshair and any duck, right now. */
static float closest_duck_gap(void)
{
	float best = LANE_SPAN;
	int   i;

	for (i = 0; i < DUCK_COUNT; i++) {
		float d = fabsf(crosshair_x() - duck_x(i));

		if (d < best)
			best = d;
	}
	return best;
}

/*
 * The trigger scores what the crosshair is over, and only that.
 *
 * The two moments are FOUND rather than computed: step time forward, read the
 * gap off the built world, and take the first frame inside the hit radius and
 * the first frame well outside it. So the check knows nothing about how the aim
 * or the conveyor are worked out — only what the booth currently looks like —
 * and cannot pass by reproducing either.
 */
static void test_trigger_scores_what_it_is_over(void)
{
	float hit_t  = -1.0f;
	float miss_t = -1.0f;
	int   ms;

	for (ms = 0; ms < 9000 && (hit_t < 0.0f || miss_t < 0.0f); ms += 16) {
		float gap;

		run_to((float)ms / 1000.0f);
		gap = closest_duck_gap();

		/* Comfortably inside, so a millimetre of rounding between the
		 * world and the image cannot flip the verdict. */
		if (hit_t < 0.0f && gap < HIT_RADIUS * 0.5f)
			hit_t = (float)ms / 1000.0f;
		/* Comfortably outside, for the same reason. */
		if (miss_t < 0.0f && gap > HIT_RADIUS * 3.0f)
			miss_t = (float)ms / 1000.0f;
	}
	assert(hit_t >= 0.0f && miss_t >= 0.0f);

	/* A fresh game, then the shot that misses: no score, and the game says
	 * it hit nothing. */
	run_to(miss_t);
	assert(scene_script_call(&w, &catalog_api, "ducks-score", 0) == 0);
	assert(scene_script_call(&w, &catalog_api, "ducks-pull-trigger!",
				 (int32_t)(miss_t * 1000.0f)) == 0);
	assert(scene_script_call(&w, &catalog_api, "ducks-score", 0) == 0);
	assert(scene_script_call(&w, &catalog_api, "ducks-last-hit", 0) == -1);

	/* And the shot that lands: one point, and a real duck named. */
	run_to(hit_t);
	assert(scene_script_call(&w, &catalog_api, "ducks-score", 0) == 0);
	assert(scene_script_call(&w, &catalog_api, "ducks-pull-trigger!",
				 (int32_t)(hit_t * 1000.0f)) == 1);
	assert(scene_script_call(&w, &catalog_api, "ducks-score", 0) == 1);
	assert(scene_script_call(&w, &catalog_api, "ducks-last-hit", 0) >= 0);
	assert(scene_script_call(&w, &catalog_api, "ducks-last-hit", 0)
	       < DUCK_COUNT);
}

/*
 * Opening the booth again is a fresh game: the (on-load ...) hook is
 * ducks-reset, so a score carried over from the last visit would be a project
 * that does not reset, which is the bug chess's own reload check exists for.
 */
static void test_reload_is_a_fresh_game(void)
{
	run_to(0.0f);
	assert(scene_script_call(&w, &catalog_api, "ducks-pull-trigger!", 0)
	       >= 0);

	assert(open_ducks() == DUCKS_ENTITY_COUNT);
	assert(scene_script_call(&w, &catalog_api, "ducks-score", 0) == 0);
	assert(scene_script_call(&w, &catalog_api, "ducks-last-hit", 0) == -1);
}

int main(void)
{
	mem_init();
	log_init();
	asset_init();         /* the real catalog the three scripts land in */
	script_init();        /* loads scene_script.scm (scene-build) */
	entity_script_init(); /* the entity-* primitives a script clause calls */
	/* Bind the catalog before the project source is evaluated: its
	 * top-level script-define! calls are what put the scripts in it. */
	scene_script_bind_catalog(&catalog_api, &mut_api);
	scene_script_init();
	world_reset(&w);

	subsystem_manager_init(&mgr, t_subsystems);
	project_host_plugin_entry(&mgr);
	g_ducks = project_host_eval(DUCKS_SCM);
	assert(g_ducks >= 0);
	assert(game_count() == 1);
	assert(game_find("Ducks") == g_ducks);

	test_booth_builds();
	test_rank_starts_evenly_spaced();
	test_conveyor_carries_the_rank();
	test_conveyor_wraps_seamlessly();
	test_aim_sweeps_the_whole_lane();
	test_trigger_scores_what_it_is_over();
	test_reload_is_a_fresh_game();

	printf("ducks_test: ok\n");
	return 0;
}
