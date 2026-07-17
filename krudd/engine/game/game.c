/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * game — the launcher's scene registry. Holds the registered {name, load} pairs,
 * loads one by index, and (in the browser) bridges to the shell overlay: each
 * registration injects a button, and the exported krudd_load_game is what those
 * buttons call. The registry itself is plain C and host-testable; only the
 * DOM bridge is emscripten-only.
 */
#include "game.h"

#include <stddef.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#define GAME_MAX 16

static struct {
	const char *name;
	void      (*load)(void);
} g_games[GAME_MAX];

static int g_count;

#ifdef __EMSCRIPTEN__
/* Editor-chrome toggle (plugin_abi.c, main module): reset per game load. */
void krudd_set_editor_chrome(int on);

/*
 * Append a launcher button for a freshly registered game. The button calls back
 * into the exported krudd_load_game with this game's index; UTF8ToString marshals
 * the C name into a JS string. A missing host element (an older shell) is a safe
 * no-op.
 */
EM_JS(void, game_launcher_add, (const char *name, int idx), {
	var host = document.getElementById('launcher-games');
	if (!host)
		return;
	var b = document.createElement('button');
	b.className = 'launcher-btn';
	b.textContent = UTF8ToString(name);
	b.onclick = function () { Module._krudd_load_game(idx); };
	host.appendChild(b);
})

/* Hide the launcher overlay once a game has been chosen. */
EM_JS(void, game_launcher_hide, (void), {
	var el = document.getElementById('launcher');
	if (el)
		el.classList.add('hidden');
})
#endif

void game_register(const char *name, void (*load)(void))
{
	if (g_count >= GAME_MAX || !name || !load)
		return;
	g_games[g_count].name = name;
	g_games[g_count].load = load;
#ifdef __EMSCRIPTEN__
	game_launcher_add(name, g_count);
#endif
	g_count++;
}

int game_count(void)
{
	return g_count;
}

void game_load(int index)
{
	if (index >= 0 && index < g_count && g_games[index].load)
		g_games[index].load();
}

#ifdef __EMSCRIPTEN__
/*
 * The launcher's entry point from JS: load the chosen game, then dismiss the
 * overlay. Exported to Module._krudd_load_game (see the shell's button wiring).
 */
EMSCRIPTEN_KEEPALIVE void krudd_load_game(int index)
{
	/*
	 * Every scene starts in the editor by default; a game that wants a clean
	 * play view turns the chrome off from its load callback, which runs
	 * inside game_load below. Resetting here means leaving a chrome-less game
	 * for an ordinary one restores the editor.
	 */
	krudd_set_editor_chrome(1);
	game_load(index);
	game_launcher_hide();
}
#endif
