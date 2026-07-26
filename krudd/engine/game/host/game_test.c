/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * game — the launcher registry, host-side. Registration counts up, game_load
 * runs the right callback, an out-of-range index is inert, game_find resolves a
 * name (case-insensitively) to its slot, and game_boot_default loads by name.
 */
#include "game.h"

#include <assert.h>
#include <stdio.h>

static int g_loaded = -1;

static void load_a(void) { g_loaded = 10; }
static void load_b(void) { g_loaded = 20; }

int main(void)
{
	int ia, ib;

	/* Registration hands back the slot game_load will use to pick it. */
	ia = game_register("A", load_a);
	ib = game_register("B", load_b);
	assert(game_count() == 2);
	assert(ia == 0 && ib == 1);

	/* Nothing loaded yet: no game is active. */
	assert(game_active_index() == -1);

	game_load(0);
	assert(g_loaded == 10);
	assert(game_active_index() == 0);
	game_load(1);
	assert(g_loaded == 20);
	assert(game_active_index() == 1);

	/* Out of range and negative: no callback runs, active index stands. */
	game_load(2);
	assert(g_loaded == 20);
	assert(game_active_index() == 1);
	game_load(-1);
	assert(g_loaded == 20);
	assert(game_active_index() == 1);

	/* NULL args are ignored, not registered, and report no slot. */
	assert(game_register(NULL, load_a) == -1);
	assert(game_register("C", NULL) == -1);
	assert(game_count() == 2);

	/* game_find resolves a name to its slot, case-insensitively; a miss and a
	 * NULL name both report no slot. */
	assert(game_find("A") == 0);
	assert(game_find("b") == 1);
	assert(game_find("Z") == -1);
	assert(game_find(NULL) == -1);

	/* game_boot_default loads the named game and hands back its index; an
	 * unknown or NULL name loads nothing and leaves the active game standing. */
	assert(game_boot_default("a") == 0);
	assert(g_loaded == 10);
	assert(game_active_index() == 0);
	assert(game_boot_default("nope") == -1);
	assert(g_loaded == 10);
	assert(game_active_index() == 0);
	assert(game_boot_default(NULL) == -1);
	assert(game_active_index() == 0);

	printf("game_test: ok\n");
	return 0;
}
