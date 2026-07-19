/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * chess — the scene end to end against the real image. It boots s7, registers
 * the scene-* primitives, then builds the embedded chess scene into a world the
 * way the plugin's load does (scene_script_build) and checks the result is the
 * standard opening position: the right entity count, and the key pieces sitting
 * on the right squares by name. No GPU and no asset catalog are needed — the
 * mesh/material paths resolve to "unbound", which still spawns every named
 * entity, so the layout is fully checkable headless. This slice has no rules, so
 * there is nothing else to drive; a later move-logic slice would add its own
 * checks the way tictactoe_test drives clicks.
 */
#include "world.h"
#include "scene_script.h"

#include "script.h"
#include "log.h"

#include "chess_scene_scm.h"

#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* One world instance reused across the checks; too big for the stack. */
static struct world w;

/*
 * Total entities the scene spawns: a camera, a ground plane, the board slab, 64
 * square tiles, 32 pieces, and the two kings' crosses (two boxes each). Its
 * fingerprint — it moves only when scene.scm gains or loses an entity.
 */
#define CHESS_ENTITY_COUNT 103

/* #t when a live entity named NAME exists — the piece-on-square check. */
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

/* The scene builds to the full opening set, every entity accounted for. */
static void test_scene_builds(void)
{
	int32_t n;

	world_reset(&w);
	n = scene_script_build(&w, NULL, CHESS_SCENE_SCM);
	assert(n == CHESS_ENTITY_COUNT);
}

/*
 * The pieces stand in the standard opening position: kings on e, the white
 * queen on d1 (her own colour), rooks in the corners, and a full pawn rank in
 * front of each army. A representative sample pins the layout without listing
 * all 32.
 */
static void test_starting_position(void)
{
	world_reset(&w);
	assert(scene_script_build(&w, NULL, CHESS_SCENE_SCM)
	       == CHESS_ENTITY_COUNT);

	/* White back rank: R N B Q K B N R on rank 1. */
	assert(has_named("wR-a1") && has_named("wN-b1") && has_named("wB-c1"));
	assert(has_named("wQ-d1") && has_named("wK-e1"));
	assert(has_named("wB-f1") && has_named("wN-g1") && has_named("wR-h1"));

	/* Black mirrors it on rank 8, king still on the e file. */
	assert(has_named("bR-a8") && has_named("bQ-d8") && has_named("bK-e8"));
	assert(has_named("bR-h8"));

	/* Full pawn ranks: rank 2 white, rank 7 black. */
	assert(has_named("wP-a2") && has_named("wP-h2"));
	assert(has_named("bP-a7") && has_named("bP-h7"));

	/* The board itself and the camera are present. */
	assert(has_named("Camera") && has_named("board-base"));
	assert(has_named("sq-a1") && has_named("sq-h8"));

	/* No stray piece on an empty middle rank. */
	assert(!has_named("wP-a4"));
}

/* Rebuilding after a reset yields the same count — the build is deterministic. */
static void test_rebuild_is_stable(void)
{
	int32_t a, b;

	world_reset(&w);
	a = scene_script_build(&w, NULL, CHESS_SCENE_SCM);
	world_reset(&w);
	b = scene_script_build(&w, NULL, CHESS_SCENE_SCM);
	assert(a == b && a == CHESS_ENTITY_COUNT);
}

int main(void)
{
	log_init();
	script_init();       /* loads scene_script.scm (scene-build) */
	scene_script_init(); /* registers the scene-* host primitives */

	test_scene_builds();
	test_starting_position();
	test_rebuild_is_stable();

	printf("chess_test: ok\n");
	return 0;
}
