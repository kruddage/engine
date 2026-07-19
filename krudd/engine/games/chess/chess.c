/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * chess — a built-in game: the standard set (its meshes and PBR materials staged
 * by games/chess/scene.scm) made playable by games/chess/rules.scm. Like
 * tictactoe the plugin stays thin — it owns no geometry and no rules, only the
 * wiring: at register time it loads the rules into the shared image and offers
 * "Chess" on the launcher; when chosen, chess_load clears the world, builds the
 * scene, and starts a fresh game. Each frame the tick watches the engine's ray
 * pick and hands a freshly-clicked entity to the rules, which run the two-click
 * select→move flow. Board state, turn order and moves all live in rules.scm — a
 * game is authored data and Scheme on top of the engine, not C.
 *
 * This slice is free movement + capture + turn alternation; full legality (check,
 * castling, en passant, promotion) is a later slice layered on the same plumbing.
 */
#include "entity_api.h"
#include "subsystem_manager.h"
#include "game.h"
#include "script.h"

#include "chess_scene_scm.h"
#include "chess_rules_scm.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __EMSCRIPTEN__
/* Editor-chrome toggle (plugin_abi.c, main module). */
void krudd_set_editor_chrome(int on);
#endif

/* Resolved before register() so init/tick/load can reach the world and rules. */
static const struct entity_api *g_scene;

/* Last selection dispatched, so a held selection fires the rules only once. */
static int32_t g_last_sel = -1;

static void chess_init(void)
{
	/*
	 * Load the rules into the shared image once. The board is NOT built here —
	 * the launcher builds it via chess_load when the game is chosen, so boot
	 * lands on the menu, not a board.
	 */
	script_eval(CHESS_RULES_SCM);
}

/* Launcher entry: clear whatever was showing, build the set, start a game. */
static void chess_load(void)
{
	if (!g_scene)
		return;
#ifdef __EMSCRIPTEN__
	/*
	 * A set to play, not a scene to edit: drop the editor chrome (panels and
	 * the selection gizmo) so the board fills the frame. Click-to-pick still
	 * runs, so pieces and squares still register their clicks.
	 */
	krudd_set_editor_chrome(0);
#endif
	if (g_scene->clear_world)
		g_scene->clear_world();
	if (g_scene->build_scene_scm)
		g_scene->build_scene_scm(CHESS_SCENE_SCM);
	if (g_scene->dispatch_scm)
		g_scene->dispatch_scm("chess-reset", 0);
	g_last_sel = -1;
}

/*
 * Forward the click edge to the rules. The engine's existing ray pick already
 * turns a viewport click into the selected entity; when that selection changes
 * to a new entity, hand its id to chess-on-selected with the world bound. Whether
 * it names a piece to pick up, a destination square, or something else is the
 * rules' decision — this side only detects the edge, so a held selection fires
 * once. Two such edges (a piece, then where it goes) make one move.
 */
static void chess_tick(void)
{
	int32_t sel;

	if (!g_scene || !g_scene->get_selected || !g_scene->dispatch_scm)
		return;
	sel = g_scene->get_selected();
	if (sel == g_last_sel)
		return;
	g_last_sel = sel;
	if (sel >= 0)
		g_scene->dispatch_scm("chess-on-selected", sel);
}

static const struct subsystem chess_desc = {
	.name = "chess",
	.init = chess_init,
	.tick = chess_tick,
};

void chess_plugin_entry(struct subsystem_manager *mgr)
{
	/* The "scene" api is the entity plugin, registered before this one. */
	g_scene = subsystem_manager_get_api(mgr, "scene");
	subsystem_manager_register(mgr, &chess_desc);
	/* Offer the game on the launcher rather than building it at boot. */
	game_register("Chess", chess_load);
}
