/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * game — the registry of loadable scenes. Holds the registered {name, load}
 * pairs, resolves a name to a slot, loads one by index, and reports which one is
 * active. Plain C and host-testable; the one browser-only line is the hide that
 * takes the page's picker down once a boot has landed on a scene.
 *
 * The picker itself is not drawn from here. The page lists the projects the
 * build shipped (assets/projects.json, read by shell.html.in) and a pick there
 * navigates to ?game=<name> rather than calling in, so this registry is what
 * that name resolves against — not what the menu is made of. Registering
 * therefore injects nothing into the DOM, and a project the image does not
 * carry needs no entry here to be reachable: the page fetches its source and
 * brings it in through project_host_load, the same door a .scm off the disk
 * comes in by.
 */
#include <host/game.h>

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
/* Take the picker down once a boot has landed on a scene. A missing element (an
 * older shell) is a safe no-op. */
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
	g_count++;
	return index;
}

int game_rename(int index, const char *name)
{
	if (index < 0 || index >= g_count || !name)
		return -1;
	g_games[index].name = name;
	return 0;
}

int game_count(void)
{
	return g_count;
}

/*
 * ASCII case-insensitive equality. The launcher label is a plain-ASCII string
 * literal and the boot request comes from a URL query, so a locale-free compare
 * is enough to let a lowercase ?game= match the capitalised label a game
 * registered under — and it drags in no ctype/locale dependency the WASM build
 * would otherwise carry.
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

	if (index < 0 || !g_games[index].load)
		return -1;
	game_load(index);
#ifdef __EMSCRIPTEN__
	/*
	 * Land on the scene, not the picker: the page opened ON this project, so
	 * the overlay it was showing while the module downloaded is something it
	 * was passing through. Nothing reopens it from here — the "load project"
	 * control in the corner is how a player gets it back.
	 */
	game_launcher_hide();
#endif
	return index;
}
