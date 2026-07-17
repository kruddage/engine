/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * game — the launcher registry, host-side. Registration counts up, game_load
 * runs the right callback, and an out-of-range index is inert.
 */
#include "game.h"

#include <assert.h>
#include <stdio.h>

static int g_loaded = -1;

static void load_a(void) { g_loaded = 10; }
static void load_b(void) { g_loaded = 20; }

int main(void)
{
	game_register("A", load_a);
	game_register("B", load_b);
	assert(game_count() == 2);

	game_load(0);
	assert(g_loaded == 10);
	game_load(1);
	assert(g_loaded == 20);

	/* Out of range and negative: no callback runs, last load stands. */
	game_load(2);
	assert(g_loaded == 20);
	game_load(-1);
	assert(g_loaded == 20);

	/* NULL args are ignored, not registered. */
	game_register(NULL, load_a);
	game_register("C", NULL);
	assert(game_count() == 2);

	printf("game_test: ok\n");
	return 0;
}
