/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * project_host — host side of the project layer (see project/project_host.h for
 * the design, and project.scm for the form).
 *
 * Three seams, and nothing else: the (game-register! ...) primitive, which puts
 * a Scheme procedure on the launcher through one shared C trampoline; the
 * per-frame tick, which gates on the loaded project and hands the frame to the
 * image with the live world bound; and project_host_eval, the door a project
 * source comes in by.
 */
#include <project/project_host.h>

#include <abi/entity_api.h>
#include <core/script.h>
#include <core/subsystem_manager.h>
#include <host/game.h>
#include <log/log.h>

#include "s7.h"

#include "project_scm.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * How many projects may hold a launcher entry at once. Below game.c's own
 * GAME_MAX on purpose — the registry is shared with whatever plugins registered
 * too, and a project asking for the last slot should be told so here, where the
 * refusal can name it.
 */
#define PROJECT_MAX 8

/* Longest display name a project may register. Copied, so it is bounded. */
#define PROJECT_NAME_MAX 48

/* Fits "project-load-" plus any int the registry can hand back. */
#define PROJECT_SYM_MAX 32

/* The image procedure one frame of a loaded project is dispatched through. */
#define PROJECT_TICK_FN "project-host-tick"

/* The global a registered load thunk is bound to, suffixed with its index. */
#define PROJECT_LOAD_PREFIX "project-load-"

/*
 * The "scene" api (the entity plugin), resolved at plugin entry. This is the
 * only way into the image with the live world bound, so with no scene subsystem
 * a project registers and never runs — the same degradation a game plugin has
 * always had, and the same NULL guards.
 */
static const struct entity_api *g_scene;

/*
 * The projects holding a launcher entry. `name` is this module's own copy:
 * game_register keeps the pointer it is given for the life of the process, and
 * the string it arrives as belongs to the s7 heap. `index` is the launcher slot
 * — the key the image knows a project by, and what the trampoline recovers.
 */
struct project_slot {
	char name[PROJECT_NAME_MAX];
	int  index;
};

static struct project_slot g_slots[PROJECT_MAX];

static int g_slot_count;

/* True when launcher index IDX is one this module registered. */
static int slot_is_ours(int idx)
{
	int i;

	for (i = 0; i < g_slot_count; i++)
		if (g_slots[i].index == idx)
			return 1;
	return 0;
}

/* The image global holding the load thunk registered for launcher slot IDX. */
static void load_symbol(char *buf, size_t cap, int idx)
{
	snprintf(buf, cap, "%s%d", PROJECT_LOAD_PREFIX, idx);
}

/*
 * The load callback every project shares. game_load sets the active index
 * before running this (game.h), so the launcher slot it fires for is exactly
 * game_active_index() — which is also the key the image stores its project
 * under, so no per-project trampoline is needed and the registry keeps its
 * plain (name, void (*)(void)) shape.
 *
 * Dispatched by name rather than called as a value because the thunk has to run
 * with the live world bound: a project's load clears the world and builds its
 * scene, and dispatch_scm is what binds one for the span of a call.
 */
static void project_load_trampoline(void)
{
	char sym[PROJECT_SYM_MAX];
	int  idx = game_active_index();

	if (!slot_is_ours(idx)) {
		LOG_WARN("project: load callback fired for slot %d, which is "
			 "not a project", idx);
		return;
	}
	if (!g_scene || !g_scene->dispatch_scm) {
		LOG_WARN("project: no scene subsystem to load into");
		return;
	}
	load_symbol(sym, sizeof(sym), idx);
	g_scene->dispatch_scm(sym, 0);
}

/*
 * Claim a launcher slot for NAME, returning its index or -1.
 *
 * A name already on the launcher is a reload when it is one of ours: the same
 * slot is handed back and the caller rebinds its thunk, so re-evaluating a
 * project's source replaces it in place instead of stacking a second button
 * beside it — the same "redefinition replaces" the *-define! primitives settled
 * on, for the same reason (re-evaluating a source is how a project reloads).
 * A name belonging to something else on the launcher is refused: the entry that
 * is there has its own load callback, which this module cannot replace, so
 * accepting the registration would leave a project that never loads.
 */
static int claim_slot(const char *name)
{
	size_t n = strlen(name);
	int    found = game_find(name);
	int    idx, i;

	if (found >= 0) {
		if (slot_is_ours(found))
			return found;
		LOG_WARN("project: \"%s\" is already on the launcher", name);
		return -1;
	}
	if (n >= PROJECT_NAME_MAX) {
		LOG_WARN("project: display name is too long (%u bytes)",
			 (unsigned)n);
		return -1;
	}
	if (g_slot_count >= PROJECT_MAX) {
		LOG_WARN("project: no room for \"%s\" (%d projects registered)",
			 name, g_slot_count);
		return -1;
	}
	i = g_slot_count;
	memcpy(g_slots[i].name, name, n + 1);
	idx = game_register(g_slots[i].name, project_load_trampoline);
	if (idx < 0) {
		g_slots[i].name[0] = '\0';
		LOG_WARN("project: the launcher registry refused \"%s\"", name);
		return -1;
	}
	g_slots[i].index = idx;
	g_slot_count++;
	return idx;
}

/*
 * (game-register! "name" thunk) -> the launcher index, or -1.
 *
 * Put THUNK on the launcher under NAME: when the player picks that entry, THUNK
 * runs with the live world bound, so it may clear the world and build a scene.
 * This is game_register() with a Scheme procedure in place of a C callback, and
 * it is the only thing the registry had to grow to host a project — one
 * trampoline serves every entry registered this way, and game/host keeps no
 * knowledge of the interpreter.
 *
 * THUNK is bound to a generated global (project-load-<index>) rather than held
 * as a value here: that both roots it against the collector and makes it
 * reachable by dispatch_scm, which names a procedure rather than taking one.
 * Registering the same NAME again rebinds that global, which is how a reloaded
 * project keeps its launcher slot. A non-string name, a non-procedure thunk, a
 * name another game already holds, and a full registry all answer -1 and
 * register nothing.
 */
static s7_pointer sp_game_register(s7_scheme *sc, s7_pointer args)
{
	s7_pointer  name_arg = s7_car(args);
	s7_pointer  thunk    = s7_cadr(args);
	const char *name     = s7_is_string(name_arg) ? s7_string(name_arg)
						     : NULL;
	char        sym[PROJECT_SYM_MAX];
	int         idx;

	if (!name || !name[0] || !s7_is_procedure(thunk)) {
		LOG_WARN("project: game-register! wants a name and a procedure");
		return s7_make_integer(sc, -1);
	}
	idx = claim_slot(name);
	if (idx < 0)
		return s7_make_integer(sc, -1);
	load_symbol(sym, sizeof(sym), idx);
	s7_define_variable(sc, sym, thunk);
	return s7_make_integer(sc, idx);
}

/*
 * Register the primitive and load the project image. Idempotent, and separate
 * from the plugin entry so the interpreter comes up the same way whether the
 * host is driven by a subsystem manager or by a test.
 */
static void project_host_init(void)
{
	static int registered;
	s7_scheme *sc;

	if (registered)
		return;
	sc = script_s7();
	if (!sc) {
		LOG_WARN("project: no interpreter; projects cannot run");
		return;
	}
	s7_define_function(sc, "game-register!", sp_game_register, 2, 0, false,
			   "(game-register! name thunk) -> launcher index; put "
			   "a Scheme load callback on the launcher. Registering "
			   "the same name again rebinds it in place, keeping "
			   "the entry; a name another game holds is refused "
			   "(-1).");
	script_eval(PROJECT_SCM);
	registered = 1;
}

/*
 * One frame for the loaded project, and none for any other. The gate is here
 * rather than in the image so that it holds even for a project whose own hooks
 * are broken, and so no project has to remember to make the check — the whole
 * of what a game plugin's tick had to open with.
 */
static void project_tick(void)
{
	int idx = game_active_index();

	if (idx < 0 || !slot_is_ours(idx))
		return;
	if (!g_scene || !g_scene->dispatch_scm)
		return;
	g_scene->dispatch_scm(PROJECT_TICK_FN, idx);
}

static const struct subsystem project_desc = {
	.name = "project",
	.init = project_host_init,
	.tick = project_tick,
};

int32_t project_host_eval(const char *src)
{
	s7_scheme *sc;
	s7_pointer fn, res;

	if (!src)
		return -1;
	project_host_init();
	sc = script_s7();
	if (!sc)
		return -1;
	fn = s7_name_to_value(sc, "project-eval");
	if (!s7_is_procedure(fn)) {
		LOG_WARN("project: the project image is not loaded");
		return -1;
	}
	res = s7_call(sc, fn, s7_list(sc, 1, s7_make_string(sc, src)));
	return s7_is_integer(res) ? (int32_t)s7_integer(res) : -1;
}

void project_host_plugin_entry(struct subsystem_manager *mgr)
{
	/* The "scene" api is the entity plugin, registered before this one. */
	g_scene = subsystem_manager_get_api(mgr, "scene");
	subsystem_manager_register(mgr, &project_desc);
}
