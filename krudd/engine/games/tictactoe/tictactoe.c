/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * tictactoe — the first built-in game. The plugin stays thin: it owns no rules
 * and no geometry, only the wiring. At register time it loads the game's Scheme
 * rules into the shared image and builds the board scene; each frame it watches
 * the selection the engine's ray pick maintains and hands a freshly-clicked cell
 * to those rules. Board state, turn order and mark placement all live in
 * rules.scm — a game is authored data and Scheme on top of the engine, not C.
 */
#include "entity_api.h"
#include "subsystem_manager.h"
#include "script.h"

#include "tictactoe_scene_scm.h"
#include "tictactoe_rules_scm.h"

#include <stddef.h>
#include <stdint.h>

/* Resolved before register() so init/tick can reach the world and rules. */
static const struct entity_api *g_scene;

/* Last selection dispatched, so a held selection fires the rules only once. */
static int32_t g_last_sel = -1;

static void tictactoe_init(void)
{
	/* Load the rules into the shared image, then build the board scene. */
	script_eval(TICTACTOE_RULES_SCM);
	if (g_scene && g_scene->build_scene_scm)
		g_scene->build_scene_scm(TICTACTOE_SCENE_SCM);
}

/*
 * Forward the click edge to the rules. The engine's existing ray pick already
 * turns a viewport click into the selected entity; when that selection changes
 * to a new entity, hand its id to ttt-on-selected with the world bound. Whether
 * it names an empty cell, an occupied one, or something else is the rules'
 * decision — this side only detects the edge, so a held selection fires once.
 */
static void tictactoe_tick(void)
{
	int32_t sel;

	if (!g_scene || !g_scene->get_selected || !g_scene->dispatch_scm)
		return;
	sel = g_scene->get_selected();
	if (sel == g_last_sel)
		return;
	g_last_sel = sel;
	if (sel >= 0)
		g_scene->dispatch_scm("ttt-on-selected", sel);
}

static const struct subsystem tictactoe_desc = {
	.name = "tictactoe",
	.init = tictactoe_init,
	.tick = tictactoe_tick,
};

void tictactoe_plugin_entry(struct subsystem_manager *mgr)
{
	/* The "scene" api is the entity plugin, registered before this one. */
	g_scene = subsystem_manager_get_api(mgr, "scene");
	subsystem_manager_register(mgr, &tictactoe_desc);
}
