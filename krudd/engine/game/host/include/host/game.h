/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef GAME_H
#define GAME_H

/*
 * game — the registry of loadable scenes the launcher offers. A game (or the
 * procedural demo) registers a display name and a load callback at plugin-entry
 * time instead of building itself at boot; the launcher lists what is registered
 * and, on a click, loads exactly one. This is what lets several games live in one
 * build without all of them spawning into a single world at startup.
 *
 * In the browser, registering also injects a button into the shell's launcher
 * overlay, and the exported krudd_load_game(index) (see game.c) is what those
 * buttons call — so the HTML menu is driven by whatever registered, no per-game
 * wiring in the page.
 */

/*
 * Register a loadable scene. NAME is the button label (must outlive the process —
 * a string literal); LOAD builds it into the world when chosen. Returns the slot
 * INDEX game_load will use to pick it, which a game with a per-frame tick should
 * hold onto and compare against game_active_index() (see below) so its tick can
 * tell whether it is the loaded game. Returns -1, and registers nothing, past a
 * small fixed capacity or on NULL args.
 */
int  game_register(const char *name, void (*load)(void));

/* Number of registered games. */
int  game_count(void);

/*
 * Index of the registered game whose display name equals NAME, compared
 * case-insensitively (ASCII), or -1 if none matches or NAME is NULL. Lets a
 * caller pick a game by name — e.g. a boot-time default read from a URL query —
 * without depending on registration order.
 */
int  game_find(const char *name);

/* Load the game at INDEX (runs its load callback). Out-of-range is a no-op. */
void game_load(int index);

/*
 * Open the game at INDEX as the boot default, exactly as a launcher click
 * would: load it and (in the browser) dismiss the launcher overlay and show the
 * "load project" splash. An out-of-range index (including the -1 a failed
 * registration hands back) is a no-op that leaves the launcher up. Returns the
 * loaded index, or -1.
 *
 * This is the by-slot half of the pair below, and it exists because a boot
 * default is not always a name: the engine's own default is whichever project
 * the build staged, which registered itself moments earlier and reported the
 * slot it took. Resolving that back through a name would mean core knowing one.
 */
int  game_boot_index(int index);

/*
 * Open the game named NAME as the boot default — game_boot_index against
 * game_find. NULL, "", or an unregistered name is a no-op that leaves the
 * launcher up, so ?game=none is how a page opts back into the "choose a scene"
 * menu. Returns the loaded index, or -1.
 */
int  game_boot_default(const char *name);

/*
 * Index of the game the last successful game_load landed on, or -1 before any
 * load (e.g. at boot, sitting on the launcher menu). Every registered game's
 * subsystem ticks every frame regardless of what the launcher loaded, so a
 * game whose tick reaches into its own rules must gate on
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
