/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * training — the room, measured.
 *
 * This test exists for one reason: training.scm asserts that ONE WORLD UNIT IS
 * ONE METRE, and an assertion nothing reads back is a comment. So the room is
 * built through the path the engine actually takes — project_host_eval
 * registers the (project ...) form on the real launcher registry, game_load
 * runs the host's own load callback over a stand-in "scene" api — and then
 * every dimension that matters is read off the built world and checked against
 * the metres it is supposed to be.
 *
 * What it checks: the room is 6 m x 6 m under a 3 m ceiling with its floor at
 * y = 0; the walls stand at x = ±3 and z = ±3; the glowing grid is ruled at one
 * metre intervals; the flatscreen camera sits at standing eye height; and the
 * human reference really is 1.75 m tall with its feet on the floor — which,
 * because a capsule is two units tall rather than one, is the check that would
 * have caught the room being built at half scale.
 *
 * No GPU and no asset catalog: the room's two materials resolve to "unbound",
 * which still spawns every entity the scene declares. Geometry is what this is
 * about, and geometry is fully readable headless.
 */
#include <project/project_host.h>

#include <entity/world.h>
#include <entity/scene_script.h>
#include <abi/entity_api.h>
#include <core/subsystem_manager.h>
#include <host/game.h>
#include <log/log.h>

#include "training_scm.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* One world instance reused across the checks; too big for the stack. */
static struct world w;

static struct subsystem_manager mgr;

/* The launcher slot training.scm took, from project_host_eval in main. */
static int32_t g_training;

/*
 * The stand-in "scene" subsystem, exactly game/project's own test harness: the
 * three entity_api entries a project's host uses, each binding this world the
 * way entity_plugin.c binds the live one. NULL for the catalog throughout —
 * see the file comment.
 */
static int32_t t_build_scene_scm(const char *src)
{
	return scene_script_build(&w, NULL, src);
}

static int32_t t_dispatch_scm(const char *fn, int32_t arg)
{
	return scene_script_call(&w, NULL, fn, arg);
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

static void t_scene_init(void)
{
	world_reset(&w);
	scene_script_init();
}

static const struct subsystem t_subsystems[] = {
	{ .name = "scene", .api = &t_scene_api, .init = t_scene_init },
	{ NULL }
};

/*
 * Total entities the room spawns: a camera, floor and ceiling, four walls,
 * seven floor lines each way, two ruled lines around each of the four walls,
 * and the human reference. Its fingerprint — it moves only when the (scene ...)
 * clause of training.scm gains or loses an entity.
 */
#define TRAINING_ENTITY_COUNT 30

/* The room, in metres. The same numbers training.scm is written in. */
#define ROOM_SIZE      6.0f
#define ROOM_HEIGHT    3.0f
#define ROOM_HALF      3.0f
#define EYE_HEIGHT     1.6f
#define HUMAN_HEIGHT   1.75f
#define GRID_STEP      1.0f
#define LINE_WIDTH     0.02f

/*
 * A millimetre. Every dimension here is authored as a decimal literal in Scheme
 * and lands in a float, so the comparisons want a tolerance; a millimetre is
 * far tighter than any error worth catching and far looser than the rounding.
 */
#define MM 0.001f

static int near(float a, float b)
{
	return fabsf(a - b) < MM;
}

/* Open the room the way the launcher does, and answer how many entities stand. */
static uint32_t open_training(void)
{
	game_load(g_training);
	return w.count;
}

/* The entity named NAME. Asserts rather than returns a sentinel: every name
 * asked for below is one the scene is required to have, so a miss is the
 * failure, not a case to handle. */
static uint32_t id_named(const char *name)
{
	uint32_t e;

	for (e = 0; e < w.count; e++) {
		const char *n = world_entity_name(&w, e);

		if (n && strcmp(n, name) == 0)
			return e;
	}
	assert(0 && "entity not in the built room");
	return 0;
}

static const float *pos_of(const char *name)
{
	return w.local[id_named(name)].position;
}

static const float *scale_of(const char *name)
{
	return w.local[id_named(name)].scale;
}

/* The room builds, whole, and builds the same twice — the host clears before it
 * builds rather than stacking a second room on the first. */
static void test_room_builds(void)
{
	assert(open_training() == TRAINING_ENTITY_COUNT);
	assert(open_training() == TRAINING_ENTITY_COUNT);
}

/*
 * The shell is a real room: a 6 m square floor at y = 0 — the height WebXR
 * calls the floor, so the room needs no offset to be stood in — a ceiling 3 m
 * above it, and four walls at ±3 m. The floor and ceiling are planes, whose
 * authored extent is a unit quad, so their scale IS their size in metres.
 */
static void test_room_is_six_by_six_by_three(void)
{
	const float *floor_p, *floor_s, *ceil_p;

	assert(open_training() == TRAINING_ENTITY_COUNT);

	floor_p = pos_of("floor");
	floor_s = scale_of("floor");
	assert(near(floor_p[1], 0.0f));
	assert(near(floor_s[0], ROOM_SIZE) && near(floor_s[2], ROOM_SIZE));

	ceil_p = pos_of("ceiling");
	assert(near(ceil_p[1], ROOM_HEIGHT));

	assert(near(pos_of("wall-north")[2], -ROOM_HALF));
	assert(near(pos_of("wall-south")[2], ROOM_HALF));
	assert(near(pos_of("wall-west")[0], -ROOM_HALF));
	assert(near(pos_of("wall-east")[0], ROOM_HALF));

	/* A wall spans the room and stands its full height, centred half way
	 * up — a box is the unit cube, so again scale is metres. */
	assert(near(scale_of("wall-north")[0], ROOM_SIZE));
	assert(near(scale_of("wall-north")[1], ROOM_HEIGHT));
	assert(near(pos_of("wall-north")[1], ROOM_HEIGHT / 2.0f));
}

/*
 * The grid is a ruler: one line per metre, in both axes, from wall to wall.
 * Checking the spacing rather than each position is what makes this a check of
 * the convention rather than a transcription of the scene clause.
 */
static void test_grid_is_ruled_in_metres(void)
{
	static const char *along_x[] = {
		"floor-x-n3", "floor-x-n2", "floor-x-n1", "floor-x-0",
		"floor-x-p1", "floor-x-p2", "floor-x-p3",
	};
	static const char *along_z[] = {
		"floor-z-n3", "floor-z-n2", "floor-z-n1", "floor-z-0",
		"floor-z-p1", "floor-z-p2", "floor-z-p3",
	};
	int i;

	assert(open_training() == TRAINING_ENTITY_COUNT);

	for (i = 0; i < 7; i++) {
		/* Lines along X are spaced in Z, and vice versa. */
		assert(near(pos_of(along_x[i])[2], -ROOM_HALF + i * GRID_STEP));
		assert(near(pos_of(along_z[i])[0], -ROOM_HALF + i * GRID_STEP));

		/* Each spans the room in its long axis and is a hair wide in
		 * the other. */
		assert(near(scale_of(along_x[i])[0], ROOM_SIZE));
		assert(near(scale_of(along_x[i])[2], LINE_WIDTH));
		assert(near(scale_of(along_z[i])[2], ROOM_SIZE));
		assert(near(scale_of(along_z[i])[0], LINE_WIDTH));
	}

	/* The ruled wall lines sit at chest and head height on every wall. */
	assert(near(pos_of("wall-north-y1")[1], 1.0f));
	assert(near(pos_of("wall-north-y2")[1], 2.0f));
	assert(near(pos_of("wall-east-y1")[1], 1.0f));
	assert(near(pos_of("wall-west-y2")[1], 2.0f));
}

/*
 * THE CHECK THIS PROJECT IS FOR. A capsule is authored radius 0.5 with a
 * cylinder length of 1 — total height TWO units, not one — so a 1.75 m figure
 * is scaled 0.875 in Y. Reading the height back as scale * 2 is what makes this
 * an assertion about metres rather than a restatement of the number in the
 * scene clause: if the room were ever rebuilt at half scale, or the capsule's
 * authored extent changed underneath it, the derived height is what moves.
 *
 * Its feet are on the floor, which for a centre-origin capsule means the centre
 * sits at exactly half its height.
 */
static void test_human_reference_is_human_sized(void)
{
	const float *s, *p;
	float height;

	assert(open_training() == TRAINING_ENTITY_COUNT);

	s = scale_of("human-reference");
	p = pos_of("human-reference");

	height = s[1] * 2.0f;   /* the capsule's authored height is 2 units */
	assert(near(height, HUMAN_HEIGHT));

	/* Feet on the floor: centre at half height, so the lowest point is 0. */
	assert(near(p[1], height / 2.0f));

	/* And shoulder-width rather than a pole or a barrel: the capsule is a
	 * unit across at scale 1, so this is metres directly. */
	assert(s[0] > 0.3f && s[0] < 0.6f);
	assert(near(s[0], s[2]));

	/* It stands inside the room it is a reference for. */
	assert(p[0] > -ROOM_HALF && p[0] < ROOM_HALF);
	assert(p[2] > -ROOM_HALF && p[2] < ROOM_HALF);
	assert(height < ROOM_HEIGHT);
}

/*
 * The flatscreen camera parks at the eye height a headset assumes before it has
 * measured anyone, so what a monitor shows today is what standing in the room
 * would show later. It stands inside the room, not outside looking in — this is
 * a space you are in, which is the difference between this project and chess.
 */
static void test_camera_stands_at_eye_height(void)
{
	const float *p;

	assert(open_training() == TRAINING_ENTITY_COUNT);

	p = pos_of("Camera");
	assert(near(p[1], EYE_HEIGHT));
	assert(p[0] > -ROOM_HALF && p[0] < ROOM_HALF);
	assert(p[2] > -ROOM_HALF && p[2] < ROOM_HALF);
}

/*
 * The convention itself, read back out of the image the way a C caller reads
 * any image state: dispatch_scm calls a named procedure with one integer and
 * hands back its integer result. If the engine ever adopts a different world
 * unit, this is the assert that notices.
 */
static void test_one_unit_is_one_metre(void)
{
	assert(open_training() == TRAINING_ENTITY_COUNT);
	assert(scene_script_call(&w, NULL, "training-metres-per-unit", 0) == 1);
}

int main(void)
{
	log_init();
	subsystem_manager_init(&mgr, t_subsystems);
	project_host_plugin_entry(&mgr);

	g_training = project_host_eval(TRAINING_SCM);
	assert(g_training >= 0);
	assert(game_count() == 1);
	assert(game_find("Training") == g_training);

	test_room_builds();
	test_room_is_six_by_six_by_three();
	test_grid_is_ruled_in_metres();
	test_human_reference_is_human_sized();
	test_camera_stands_at_eye_height();
	test_one_unit_is_one_metre();

	printf("training_test: ok\n");
	return 0;
}
