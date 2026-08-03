/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef GAME_H
#define GAME_H

/*
 * game — the registry of loadable scenes. A game registers a display name and a
 * load callback when its source is evaluated instead of building itself at boot;
 * exactly one is loaded, by name or by slot. This is what lets several scenes
 * live in one build without all of them spawning into a single world at startup.
 *
 * The registry is not the page's menu. The projects a build ships are listed by
 * the build (assets/projects.json) and offered by the shell, which opens one by
 * navigating to ?game=<name> — so what this holds is what such a name resolves
 * against (game_find, game_boot_default), and a name that resolves to nothing
 * here is one the page fetches out of assets/ and brings in through
 * project_host_load instead.
 */

/*
 * Register a loadable scene. NAME is what ?game= matches (must outlive the
 * process — a string literal); LOAD builds it into the world when chosen.
 * Returns the slot
 * INDEX game_load will use to pick it, which a game with a per-frame tick should
 * hold onto and compare against game_active_index() (see below) so its tick can
 * tell whether it is the loaded game. Returns -1, and registers nothing, past a
 * small fixed capacity or on NULL args.
 */
int  game_register(const char *name, void (*load)(void));

/*
 * Re-point the entry at INDEX at a new NAME, keeping its slot and its load
 * callback. Returns 0, or -1 for an out-of-range index or a NULL name, having
 * changed nothing. NAME must outlive the process, exactly as at registration.
 *
 * This is for a host that registers a slot per thing it is HANDED rather than
 * per thing it is: game/project keeps one entry for whatever project was
 * loaded off disk last, so that opening a second project replaces the first
 * rather than stacking beside it. The registry has no unregister and is not
 * getting one — a slot index is a key the rest of the engine holds
 * (game_active_index, and game/project's own table), so slots are stable for
 * the life of the process and only what stands in them may change.
 */
int  game_rename(int index, const char *name);

/* Number of registered games. */
int  game_count(void);

/*
 * Index of the registered game whose display name equals NAME, compared
 * case-insensitively (ASCII), or -1 if none matches or NAME is NULL. Lets a
 * caller pick a game by name — e.g. the boot scene read from a URL query —
 * without depending on registration order.
 */
int  game_find(const char *name);

/* Load the game at INDEX (runs its load callback). Out-of-range is a no-op. */
void game_load(int index);

/*
 * Open the game named NAME as the page's boot scene: load it and (in the
 * browser) take the picker down over it. Returns the loaded index, or -1 for
 * NULL, "", or a name nothing registered under — which loads nothing and leaves
 * the picker standing, because a name this image does not carry may still be a
 * project the build shipped as a file (the page fetches those; see
 * krudd_boot_unresolved in core/engine.c).
 *
 * By name and only by name: a boot happens because a URL asked for one, and the
 * only names there are come from the build's own project list.
 */
int  game_boot_default(const char *name);

/*
 * Index of the game the last successful game_load landed on, or -1 before any
 * load (e.g. at boot, sitting on the picker with nothing opened). Every
 * registered game's subsystem ticks every frame regardless of which one was
 * loaded, so a game whose tick reaches into its own rules must gate on
 * game_active_index() == <its own registered index> first — otherwise it
 * fires against whatever scene happens to be loaded, not just its own.
 *
 * game_load sets this BEFORE running the load callback, so a callback may read
 * it to learn which slot it was loaded through. That is what lets one callback
 * serve several entries — a host that registers a slot per thing it is handed
 * (game/project) registers one trampoline and works out which entry fired from
 * here, rather than minting a callback per slot.
 */
int  game_active_index(void);

#endif /* GAME_H */
