/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * chess — the scene end to end against the real image. It boots s7, registers
 * the scene-* primitives, then builds the embedded chess scene into a world the
 * way the plugin's load does (scene_script_build) and checks the result is the
 * standard opening position: the right entity count, and the key pieces sitting
 * on the right squares by name. No GPU and no asset catalog are needed — the
 * mesh/material paths resolve to "unbound", which still spawns every named
 * entity, so the layout is fully checkable headless. It then drives the rules the
 * way the plugin's tick does — hand a picked entity's id to chess-on-selected
 * with the world bound (scene_script_call) — over a small hand-built board, and
 * checks the two-click select→move flow: a slide, a capture, re-picking, a
 * deselect, and turn alternation. No GPU or browser is needed.
 */
#include "world.h"
#include "scene_script.h"

#include "script.h"
#include "log.h"

#include "chess_scene_scm.h"
#include "chess_rules_scm.h"

#include <assert.h>
#include <math.h>
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

/*
 * A tiny hand-built board for driving the rules, entity ids 0..3 in this order:
 *   0  wP-e2   a white pawn on e2   (world 0.5, 2.5)
 *   1  sq-e4   an empty target tile (world 0.5, 0.5)
 *   2  bP-e7   a black pawn on e7   (world 0.5, -2.5)   — a capture target
 *   3  wN-b1   a white knight on b1 (world -2.5, 3.5)   — a second white piece
 * The rules read names and positions and move by id, so no meshes or catalog are
 * needed; the y a piece lands at is the authored 0.03.
 */
static const char *RULES_BOARD =
	"(scene c"
	"  (entity (name \"wP-e2\") (at 0.5 0.03 2.5))"
	"  (entity (name \"sq-e4\") (at 0.5 0.02 0.5))"
	"  (entity (name \"bP-e7\") (at 0.5 0.03 -2.5))"
	"  (entity (name \"wN-b1\") (at -2.5 0.03 3.5)))";

/* Click the entity whose id is `id`, returning chess-on-selected's code. */
static int32_t click(int32_t id)
{
	return scene_script_call(&w, NULL, "chess-on-selected", id);
}

/* Poll *chess-turn*: 1 white to move, 2 black. */
static int32_t turn(void)
{
	return scene_script_call(&w, NULL, "chess-turn", 0);
}

/* #t when entity `id`'s local position is (x, *, z) within a hair. */
static int at_xz(int32_t id, float x, float z)
{
	const float *p = w.local[id].position;

	return fabsf(p[0] - x) < 1e-4f && fabsf(p[2] - z) < 1e-4f;
}

static void reset_rules_board(void)
{
	world_reset(&w);
	assert(scene_script_build(&w, NULL, RULES_BOARD) == 4);
	scene_script_call(&w, NULL, "chess-reset", 0);
}

/* Two clicks — pick a pawn, then an empty square — slide it and pass the turn. */
static void test_move_and_turn(void)
{
	reset_rules_board();

	assert(turn() == 1);             /* white to move */
	assert(click(0) == 0);           /* pick up wP-e2 (a selection, no move) */
	assert(at_xz(0, 0.5f, 2.5f));    /* still home until the second click */
	assert(turn() == 1);             /* selecting does not pass the turn */
	assert(click(1) == 1);           /* click sq-e4 -> slide there */
	assert(at_xz(0, 0.5f, 0.5f));    /* the pawn is on e4 now */
	assert(turn() == 2);             /* and it is black's move */
}

/* Picking an enemy piece as the destination removes it and takes its square. */
static void test_capture(void)
{
	reset_rules_board();

	assert(click(0) == 0);           /* pick up wP-e2 */
	assert(click(2) == 2);           /* click bP-e7 -> capture (code 2) */
	assert(!w.alive[2]);             /* the black pawn is gone */
	assert(at_xz(0, 0.5f, -2.5f));   /* the white pawn stands on e7 */
	assert(turn() == 2);             /* turn passed */
}

/* Clicking your own piece while one is picked re-picks it rather than moving. */
static void test_repick(void)
{
	reset_rules_board();

	assert(click(0) == 0);           /* pick up the pawn */
	assert(click(3) == 0);           /* click the knight (also white) -> re-pick */
	assert(at_xz(0, 0.5f, 2.5f));    /* the pawn did not move */
	assert(turn() == 1);             /* no move, still white */
	assert(click(1) == 1);           /* now sq-e4 moves the KNIGHT, not the pawn */
	assert(at_xz(3, 0.5f, 0.5f));    /* knight on e4 */
	assert(at_xz(0, 0.5f, 2.5f));    /* pawn untouched */
	assert(turn() == 2);
}

/* A first click on the wrong side's piece (or empty space) picks up nothing. */
static void test_wrong_side_ignored(void)
{
	reset_rules_board();

	assert(click(2) == 0);           /* black pawn, white to move -> ignored */
	assert(turn() == 1);
	assert(click(1) == 0);           /* an empty square with nothing picked -> no-op */
	assert(turn() == 1);
	assert(w.alive[2]);              /* nothing was moved or captured */
}

int main(void)
{
	log_init();
	script_init();       /* loads scene_script.scm (scene-build) */
	scene_script_init(); /* registers the scene-* host primitives */
	script_eval(CHESS_RULES_SCM); /* load the rules into the image */

	test_scene_builds();
	test_starting_position();
	test_rebuild_is_stable();
	test_move_and_turn();
	test_capture();
	test_repick();
	test_wrong_side_ignored();

	printf("chess_test: ok\n");
	return 0;
}
