/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * tictactoe — the game rules end to end against the real image. It boots s7,
 * registers the scene-* primitives, loads the embedded rules, then builds a tiny
 * world of named cells and drives clicks the way the plugin's tick does: hand a
 * cell entity's id to ttt-on-selected (with the world bound, via
 * scene_script_call) and check the board fills, marks spawn, turns alternate, and
 * occupied cells / non-cells are rejected — all without a GPU or a browser.
 *
 * A placed X is a three-entity composite (mesh-less parent + two bars); a placed
 * O is one entity. So the world's entity count after a placement tells X from O,
 * which is how the checks confirm the turn alternated (X first, then O).
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
 * Three entities with known ids: 0 = cell-0, 1 = cell-4, 2 = a non-cell. No asset
 * catalog is needed — the rules read names, and a spawned mark's mesh path simply
 * resolves to "unbound", which still creates the entities the counts assert on.
 */
static const char *CELLS =
	"(scene t"
	"  (entity (name \"cell-0\"))"
	"  (entity (name \"cell-4\"))"
	"  (entity (name \"middle-of-nowhere\")))";

int main(void)
{
	uint32_t base;
	int32_t  r;

	log_init();
	script_init();       /* loads scene_script.scm (scene-build, scene-entity-build) */
	scene_script_init(); /* registers the scene-* host primitives */
	script_eval(TICTACTOE_RULES_SCM);

	world_reset(&w);
	assert(scene_script_build(&w, NULL, CELLS) == 3);
	scene_script_call(&w, NULL, "ttt-reset", 0);
	base = w.count;                              /* the three cell entities */

	/* First click: X lands on cell 0 (parent + two bars = +3 entities). */
	r = scene_script_call(&w, NULL, "ttt-on-selected", 0);
	assert(r == 1);
	assert(w.count == base + 3);

	/* Same cell again: occupied, so nothing places and nothing spawns. */
	r = scene_script_call(&w, NULL, "ttt-on-selected", 0);
	assert(r == 0);
	assert(w.count == base + 3);

	/* Turn alternated: the next placement is an O (one entity) on cell 4. */
	r = scene_script_call(&w, NULL, "ttt-on-selected", 1);
	assert(r == 1);
	assert(w.count == base + 4);

	/* A non-cell entity is ignored outright. */
	r = scene_script_call(&w, NULL, "ttt-on-selected", 2);
	assert(r == 0);
	assert(w.count == base + 4);

	printf("tictactoe_test: ok\n");
	return 0;
}
