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
#include "audio_api.h"

#include "chess_scene_scm.h"
#include "chess_rules_scm.h"

#include <stddef.h>
#include <stdint.h>

/* Resolved before register() so init/tick/load can reach the world and rules. */
static const struct entity_api *g_scene;

/*
 * Resolved like g_scene: the "audio" subsystem registers before "chess" in the
 * engine's static table, so it is live by the time chess_plugin_entry looks it
 * up. NULL (and every call below a no-op) on a build with no audio backend,
 * exactly as g_scene's callbacks are guarded.
 */
static const struct audio_api *g_audio;

/* Last selection dispatched, so a held selection fires the rules only once. */
static int32_t g_last_sel = -1;

/*
 * This game's own slot in the launcher registry (game_register's return),
 * -1 until plugin entry runs. chess_tick compares it against
 * game_active_index() so the rules only fire while chess is the loaded
 * game — the "chess" subsystem ticks every frame regardless of what the
 * launcher loaded, and without this guard a selection made in ANY loaded
 * scene (tic-tac-toe, the editor) was handed to chess-on-selected too.
 */
static int g_my_index = -1;

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
	if (g_scene->clear_world)
		g_scene->clear_world();
	if (g_scene->build_scene_scm)
		g_scene->build_scene_scm(CHESS_SCENE_SCM);
	if (g_scene->dispatch_scm)
		g_scene->dispatch_scm("chess-reset", 0);
	g_last_sel = -1;
}

/*
 * Sound cue off chess-on-selected's return: 0 = a pick or a no-op click (silent,
 * it moved nothing), 1 = a piece slid to an empty square (a soft wood tap), 2 = a
 * capture (a sharper cue). The two built-in sounds (asset_plugin.c) need no new
 * authoring, just an assignment; a build with no audio backend leaves g_audio
 * NULL and every call a no-op.
 */
static void chess_play_sound(int32_t code)
{
	if (!g_audio || !g_audio->play_path)
		return;
	if (code == 1)
		g_audio->play_path("builtin://sound/blip", 0.6f, 0.0f, 1.0f);
	else if (code == 2)
		g_audio->play_path("builtin://sound/beep", 0.7f, 0.0f, 1.0f);
}

/*
 * Forward the click edge to the rules. The engine's existing ray pick already
 * turns a viewport click into the selected entity; when that selection changes
 * to a new entity, hand its id to chess-on-selected with the world bound. Whether
 * it names a piece to pick up, a destination square, or something else is the
 * rules' decision — this side only detects the edge, so a held selection fires
 * once. Two such edges (a piece, then where it goes) make one move; the rules'
 * return says whether that second click moved (1), captured (2), or did neither
 * (0), which is all the cue below needs.
 */
static void chess_tick(void)
{
	int32_t sel, code;

	/* Not the loaded game right now: leave whatever scene IS loaded alone. */
	if (g_my_index < 0 || game_active_index() != g_my_index)
		return;
	if (!g_scene || !g_scene->get_selected || !g_scene->dispatch_scm)
		return;
	sel = g_scene->get_selected();
	if (sel == g_last_sel)
		return;
	g_last_sel = sel;
	if (sel >= 0) {
		code = g_scene->dispatch_scm("chess-on-selected", sel);
		chess_play_sound(code);
	}
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
	g_audio = subsystem_manager_get_api(mgr, "audio");
	subsystem_manager_register(mgr, &chess_desc);
	/* Offer the game on the launcher rather than building it at boot. */
	g_my_index = game_register("Chess", chess_load);
}
