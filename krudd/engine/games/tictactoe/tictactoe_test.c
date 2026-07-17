/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * tictactoe — the game rules end to end against the real image. It boots s7,
 * registers the scene-* primitives, loads the embedded rules, then builds a board
 * of nine named cells and drives clicks the way the plugin's tick does: hand a
 * cell's id to ttt-on-selected (with the world bound, via scene_script_call). The
 * checks cover placement + turns, a row win, a full-board draw, and click-to-
 * restart — all without a GPU or a browser.
 *
 * A placed X is a three-entity composite (mesh-less parent + two bars); a placed
 * O is one entity. So an entity-count delta after a placement tells X from O,
 * which is how the checks confirm the turn alternates.
 */
#include "world.h"
#include "scene.h"
#include "scene_script.h"

#include "script.h"
#include "log.h"

#include "tictactoe_rules_scm.h"

#include <assert.h>
#include <stdio.h>
#include <stdint.h>

/* One world instance reused across the checks; too big for the stack. */
static struct world w;

/*
 * Nine cells named cell-0..cell-8 at ids 0..8 — a full board to play on. No asset
 * catalog is needed: the rules read names, and a spawned mark's mesh path simply
 * resolves to "unbound", which still creates the entities the counts assert on.
 */
static const char *BOARD =
	"(scene t"
	"  (entity (name \"cell-0\")) (entity (name \"cell-1\")) (entity (name \"cell-2\"))"
	"  (entity (name \"cell-3\")) (entity (name \"cell-4\")) (entity (name \"cell-5\"))"
	"  (entity (name \"cell-6\")) (entity (name \"cell-7\")) (entity (name \"cell-8\")))";

/* Click the cell whose entity id is `cell` (id N is cell-N here). */
static int32_t play(int32_t cell)
{
	return scene_script_call(&w, NULL, "ttt-on-selected", cell);
}

/* Poll *ttt-over*: 0 playing, 1 X won, 2 O won, 3 draw. */
static int32_t status(void)
{
	return scene_script_call(&w, NULL, "ttt-status", 0);
}

/* The X and O tallies, unpacked from ttt-score's X*1000 + O packing. */
static int32_t score_x(void)
{
	return scene_script_call(&w, NULL, "ttt-score", 0) / 1000;
}

static int32_t score_o(void)
{
	return scene_script_call(&w, NULL, "ttt-score", 0) % 1000;
}

static void reset_board(void)
{
	world_reset(&w);
	assert(scene_script_build(&w, NULL, BOARD) == 9);
	scene_script_call(&w, NULL, "ttt-reset", 0);
	/* Zero the tally too, so each check starts from a clean scoreboard. */
	scene_script_call(&w, NULL, "ttt-score-reset", 0);
}

/* Placement, occupied-cell rejection, non-cell rejection, and turn alternation. */
static void test_placement_and_turns(void)
{
	uint32_t base;

	reset_board();
	base = w.count;

	assert(play(0) == 1);            /* X on cell 0 */
	assert(w.count == base + 3);     /* X = parent + two bars */
	assert(play(0) == 0);            /* occupied — no-op */
	assert(w.count == base + 3);
	assert(play(4) == 1);            /* O on cell 4 (turn alternated) */
	assert(w.count == base + 4);     /* O = one entity */
	assert(play(99) == 0);           /* id 99 is no live entity — ignored */
	assert(status() == 0);           /* still playing */
}

/* A completed row ends the game for that player, then a click restarts. */
static void test_win_then_restart(void)
{
	reset_board();

	play(0);            /* X */
	play(3);            /* O */
	play(1);            /* X */
	play(4);            /* O */
	assert(status() == 0);
	assert(play(2) == 1);            /* X completes row 0-1-2 */
	assert(status() == 1);           /* X wins */

	/* With the game over, any click restarts: marks cleared, state fresh. */
	assert(play(0) == 1);
	assert(status() == 0);
	/* The board is empty again, so X can play cell 0 anew. */
	assert(play(0) == 1);
	assert(status() == 0);
}

/* A full board with no line is a draw. */
static void test_draw(void)
{
	reset_board();

	/* X:0 O:1 X:2 O:4 X:3 O:5 X:8 O:6 X:7 ->
	 *   X O X
	 *   X O O
	 *   O X X   (no three-in-a-row for either side) */
	play(0); play(1); play(2); play(4); play(3);
	play(5); play(8); play(6);
	assert(status() == 0);           /* not decided until the last cell */
	assert(play(7) == 1);            /* fills the board */
	assert(status() == 3);           /* draw */
}

/*
 * A win strikes the line through and credits the winner; the tally then survives
 * the restart that clears the marks. The winning move spawns X (parent + two bars)
 * plus the one-entity strike bar — four new entities — which is how the strike is
 * observed without a GPU.
 */
static void test_strike_and_score(void)
{
	uint32_t base;

	reset_board();

	play(0);            /* X */
	play(3);            /* O */
	play(1);            /* X */
	play(4);            /* O */

	base = w.count;
	assert(play(2) == 1);            /* X completes row 0-1-2 */
	assert(status() == 1);           /* X wins */
	assert(w.count == base + 4);     /* X (3) + the strike bar (1) */
	assert(score_x() == 1);          /* credited to X */
	assert(score_o() == 0);

	/* A click restarts: marks and the strike are swept, but the tally carries. */
	assert(play(0) == 1);
	assert(status() == 0);
	assert(score_x() == 1);
	assert(score_o() == 0);

	/* A second X win adds to the same tally rather than resetting it. */
	play(0); play(3); play(1); play(4); play(2);
	assert(status() == 1);
	assert(score_x() == 2);
	assert(score_o() == 0);
}

int main(void)
{
	log_init();
	script_init();       /* loads scene_script.scm (scene-build, scene-entity-build) */
	scene_script_init(); /* registers the scene-* host primitives */
	script_eval(TICTACTOE_RULES_SCM);

	test_placement_and_turns();
	test_win_then_restart();
	test_draw();
	test_strike_and_score();

	printf("tictactoe_test: ok\n");
	return 0;
}
