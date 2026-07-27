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
#include "game.h"
#include "script.h"
#include "audio_api.h"

#include "tictactoe_scene_scm.h"
#include "tictactoe_rules_scm.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

/*
 * Scoreboard bridge — the browser twin of the launcher's DOM buttons (game.c).
 * The scoreboard markup lives dormant in the shell (core/shell.html.in); these
 * reveal it for a game of tic-tac-toe and write the running tally into it. On a
 * shell too old to carry the element every getElementById misses and each call is
 * a safe no-op. ttt_scoreboard_set takes the two tallies already unpacked.
 */
EM_JS(void, ttt_scoreboard_show, (void), {
	var el = document.getElementById('ttt-scoreboard');
	if (el)
		el.classList.remove('hidden');
})

EM_JS(void, ttt_scoreboard_set, (int x, int o), {
	var ex = document.getElementById('ttt-score-x');
	var eo = document.getElementById('ttt-score-o');
	if (ex)
		ex.textContent = x;
	if (eo)
		eo.textContent = o;
})
#endif

/* Resolved before register() so init/tick/load can reach the world and rules. */
static const struct entity_api *g_scene;

/*
 * Resolved the same way as g_scene: the "audio" subsystem registers before
 * "tictactoe" in engine.c's static table, so it is already live by the time
 * tictactoe_plugin_entry looks it up. NULL (and every call below a no-op) on
 * a build with no audio backend, exactly like g_scene's callbacks are guarded.
 */
static const struct audio_api *g_audio;

/* Last selection dispatched, so a held selection fires the rules only once. */
static int32_t g_last_sel = -1;

/*
 * Last score pushed to the scoreboard, X*1000 + O as ttt-score packs it (game.c's
 * dispatch convention), so the DOM is rewritten only when the tally actually moves
 * and a fresh load re-pushes 0. -1 is "nothing shown yet".
 */
static int32_t g_last_score = -1;

/*
 * This game's own slot in the launcher registry (game_register's return),
 * -1 until plugin entry runs. tictactoe_tick compares it against
 * game_active_index() so the rules only fire while tic-tac-toe is the
 * loaded game — the "tictactoe" subsystem ticks every frame regardless of
 * what the launcher loaded, and without this guard a selection made in ANY
 * loaded scene (chess, the editor) was handed to ttt-on-selected too.
 */
static int g_my_index = -1;

static void tictactoe_init(void)
{
	/*
	 * Load the rules into the shared image once. The board is NOT built here —
	 * the launcher builds it via tictactoe_load when the game is chosen, so
	 * boot lands on the menu, not a board.
	 */
	script_eval(TICTACTOE_RULES_SCM);
}

/* Launcher entry: clear whatever was showing, build the board, reset the game. */
static void tictactoe_load(void)
{
	if (!g_scene)
		return;
	if (g_scene->clear_world)
		g_scene->clear_world();
	if (g_scene->build_scene_scm)
		g_scene->build_scene_scm(TICTACTOE_SCENE_SCM);
	if (g_scene->dispatch_scm) {
		/* A fresh match: zero the scoreboard, then clear the board. */
		g_scene->dispatch_scm("ttt-score-reset", 0);
		g_scene->dispatch_scm("ttt-reset", 0);
	}
	g_last_sel = -1;
	g_last_score = 0;
#ifdef __EMSCRIPTEN__
	ttt_scoreboard_show();
	ttt_scoreboard_set(0, 0);
#endif
}

/*
 * Poll the packed tally (ttt-score) and, when it has moved, push the unpacked
 * scores to the DOM scoreboard. The tally only ever changes as the result of a
 * click, so this runs right after a dispatched selection rather than every frame.
 * A dead interpreter returns -1, which never matches a real score, so it is skipped.
 */
static void tictactoe_refresh_score(void)
{
#ifdef __EMSCRIPTEN__
	int32_t enc;

	if (!g_scene->dispatch_scm)
		return;
	enc = g_scene->dispatch_scm("ttt-score", 0);
	if (enc >= 0 && enc != g_last_score) {
		g_last_score = enc;
		ttt_scoreboard_set(enc / 1000, enc % 1000);
	}
#endif
}

/*
 * Sound cue off the *ttt-over* transition a click just caused: BEFORE is the
 * status polled right before dispatching the click, AFTER right after, MOVED
 * is ttt-on-selected's own return (0 = no-op click, ignore). A restart click
 * (BEFORE non-zero: the game was already over) is deliberately silent — it is
 * a UI reset, not a move, so it gets no cue of its own. Otherwise: a plain
 * placement blips, a completed line beeps, and a filled board with no line
 * bursts static — the three built-in sounds (asset_plugin.c) needed no new
 * authoring, just an assignment. A dead interpreter reports -1 for both polls,
 * which is non-zero, so it reads as "was already over" and stays silent rather
 * than misfiring on garbage.
 */
static void tictactoe_play_sound(int32_t before, int32_t after, int32_t moved)
{
	if (!g_audio || !g_audio->play_path || !moved || before != 0)
		return;
	if (after == 0)
		g_audio->play_path("builtin://sound/blip", 0.6f, 0.0f, 1.0f);
	else if (after == 3)
		g_audio->play_path("builtin://sound/noise-burst", 0.5f, 0.0f, 1.0f);
	else
		g_audio->play_path("builtin://sound/beep", 0.7f, 0.0f, 1.0f);
}

/*
 * Forward the click edge to the rules. The engine's existing ray pick already
 * turns a viewport click into the selected entity; when that selection changes
 * to a new entity, hand its id to ttt-on-selected with the world bound. Whether
 * it names an empty cell, an occupied one, or something else is the rules'
 * decision — this side only detects the edge, so a held selection fires once.
 * *ttt-over* is polled once before and once after the dispatch (both cheap,
 * gated the same way the score refresh below already is) so the sound cue can
 * tell a placement from a win from a draw without the rules needing to know
 * anything about audio.
 */
static void tictactoe_tick(void)
{
	int32_t sel, before, after, moved;

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
		before = g_scene->dispatch_scm("ttt-status", 0);
		moved  = g_scene->dispatch_scm("ttt-on-selected", sel);
		after  = g_scene->dispatch_scm("ttt-status", 0);
		tictactoe_play_sound(before, after, moved);
		tictactoe_refresh_score();
	}
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
	g_audio = subsystem_manager_get_api(mgr, "audio");
	subsystem_manager_register(mgr, &tictactoe_desc);
	/* Offer the game on the launcher rather than building it at boot. */
	g_my_index = game_register("Tic-Tac-Toe", tictactoe_load);
}
