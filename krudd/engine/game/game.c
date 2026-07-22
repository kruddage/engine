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

/*
 * Index the last game_load call landed on, -1 before any load. Every
 * registered game's subsystem ticks each frame regardless of which one the
 * launcher loaded (subsystem_manager has no notion of "paused"), so this is
 * what game_active_index() hands back for a tick to gate on.
 */
static int g_active = -1;

#ifdef __EMSCRIPTEN__
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

int game_register(const char *name, void (*load)(void))
{
	int index;

	if (g_count >= GAME_MAX || !name || !load)
		return -1;
	index = g_count;
	g_games[index].name = name;
	g_games[index].load = load;
#ifdef __EMSCRIPTEN__
	game_launcher_add(name, index);
#endif
	g_count++;
	return index;
}

int game_count(void)
{
	return g_count;
}

/*
 * ASCII case-insensitive equality. The launcher label is a plain-ASCII string
 * literal and the boot request comes from a URL query, so a locale-free compare
 * is enough to let ?game=chess match the "Chess" a game registered under — and
 * it drags in no ctype/locale dependency the WASM build would otherwise carry.
 */
static int name_eq_ci(const char *a, const char *b)
{
	for (; *a && *b; a++, b++) {
		int ca = *a;
		int cb = *b;

		if (ca >= 'A' && ca <= 'Z')
			ca += 'a' - 'A';
		if (cb >= 'A' && cb <= 'Z')
			cb += 'a' - 'A';
		if (ca != cb)
			return 0;
	}
	return *a == *b;
}

int game_find(const char *name)
{
	int i;

	if (!name)
		return -1;
	for (i = 0; i < g_count; i++)
		if (g_games[i].name && name_eq_ci(g_games[i].name, name))
			return i;
	return -1;
}

void game_load(int index)
{
	if (index >= 0 && index < g_count && g_games[index].load) {
		g_active = index;
		g_games[index].load();
	}
}

int game_active_index(void)
{
	return g_active;
}

int game_boot_default(const char *name)
{
	int index = game_find(name);

	if (index < 0)
		return -1;
	game_load(index);
#ifdef __EMSCRIPTEN__
	/*
	 * Land on the scene, not the overlay: the same hide the click path runs,
	 * so a boot default and a launcher pick leave the page in one state. The
	 * menu button (shell.html.in) still reopens the launcher to pick another.
	 */
	game_launcher_hide();
#endif
	return index;
}

#ifdef __EMSCRIPTEN__
/*
 * The launcher's entry point from JS: load the chosen game, then dismiss the
 * overlay. Exported to Module._krudd_load_game (see the shell's button wiring).
 */
EMSCRIPTEN_KEEPALIVE void krudd_load_game(int index)
{
	game_load(index);
	game_launcher_hide();
}
#endif
