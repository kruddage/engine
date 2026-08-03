/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * project_host — host side of the project layer (see project/project_host.h for
 * the design, and project.scm for the form).
 *
 * Four seams, and nothing else: the (game-register! ...) primitive, which puts
 * a Scheme procedure on the launcher through one shared C trampoline; the
 * per-frame tick, which gates on the loaded project and hands the frame to the
 * image with the live world bound; project_host_eval, the door a project source
 * comes in by; and project_host_load, the same door with "and open it" on the
 * end, which is what the shell's Load Project control reaches through.
 */
#include <project/project_host.h>

#include <abi/entity_api.h>
#include <core/script.h>
#include <core/subsystem_manager.h>
#include <host/game.h>
#include <log/log.h>

#include "s7.h"

#include "project_scm.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

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

/*
 * The launcher entry that belongs to the load-a-project-from-outside door
 * (project_host_load), and whether a registration happening right now came in
 * through it. -1 until the first such load.
 *
 * A project that arrives at runtime is not a project this build carries: it is
 * whatever the player last opened, and there is one of those. So the door owns
 * exactly one entry and renames it in place for each new project, rather than
 * leaving a trail of buttons for sources that are no longer on screen and, past
 * PROJECT_MAX, refusing the ninth one outright. The staged project and anything
 * else registered at boot keep their own entries — they are what the build
 * ships, and nothing loaded later displaces them.
 */
static int g_door_index = -1;

static int g_through_door;

/* The slot record for launcher index IDX, or NULL if IDX is not ours. */
static struct project_slot *slot_for(int idx)
{
	int i;

	for (i = 0; i < g_slot_count; i++)
		if (g_slots[i].index == idx)
			return &g_slots[i];
	return NULL;
}

/* True when launcher index IDX is one this module registered. */
static int slot_is_ours(int idx)
{
	return slot_for(idx) != NULL;
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
 * Take over the door's own launcher entry for NAME, returning its index.
 *
 * The registry holds this very buffer as the entry's display name (it keeps the
 * pointer it was registered with), so writing the new name into it IS the
 * rename as far as game_find is concerned; game_rename is what makes the
 * launcher button on the page agree, and it is the reason the registry grew
 * that call at all.
 */
static int rename_door_slot(struct project_slot *slot, const char *name,
			    size_t n)
{
	memcpy(slot->name, name, n + 1);
	game_rename(slot->index, slot->name);
	return slot->index;
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
 *
 * A registration arriving through project_host_load — a project the player just
 * opened — takes the door's one entry instead of a fresh one, renaming it (see
 * g_door_index). The name check above still comes first, so opening a project
 * that is already on the launcher, the staged one included, reuses the entry it
 * already has rather than minting a duplicate of it under the door's.
 */
static int claim_slot(const char *name)
{
	size_t              n = strlen(name);
	int                 found = game_find(name);
	struct project_slot *door;
	int                 idx, i;

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
	door = g_through_door ? slot_for(g_door_index) : NULL;
	if (door)
		return rename_door_slot(door, name, n);
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
	if (g_through_door)
		g_door_index = idx;
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

int32_t project_host_load(const char *src)
{
	int32_t idx;

	/*
	 * The flag spans the eval rather than being an argument to it because
	 * the registration it governs happens several frames of interpreter
	 * down — the (project ...) macro reaching game-register! — and
	 * threading a "this one is the player's" argument through the image
	 * would put a policy of the C half into the Scheme half, where a
	 * project could see it. A source declaring several project forms
	 * therefore collapses onto the one entry, keeping the last: the door
	 * owns one, and that is the rule whichever way the source is shaped.
	 */
	g_through_door = 1;
	idx = project_host_eval(src);
	g_through_door = 0;
	if (idx < 0)
		return -1;
	game_load((int)idx);
	return idx;
}

#ifdef __EMSCRIPTEN__
/*
 * The page's end of project_host_load: SRC is a NUL-terminated project source
 * in the module's memory, and the answer is the launcher index it opened on, or
 * -1 for a source that did not register a project. Exported to
 * Module._krudd_load_project, the way game.c exports krudd_load_game.
 *
 * A refusal is a return value and not an exception on purpose. A project is
 * user input — the whole point of the button that reaches this is that anyone
 * may point it at any .scm — so "that file was not a project" is an ordinary
 * answer the page reports, with the engine still running the project it already
 * had. The reason is on the engine log, which project.scm has already written.
 */
EMSCRIPTEN_KEEPALIVE int32_t krudd_load_project(const char *src)
{
	return project_host_load(src);
}

/*
 * Install window.kruddRunProject(text) -> launcher index, the one call the
 * shell makes to open a project it has read off disk or fetched from assets/.
 *
 * The marshalling lives here, inside the module's own JS scope, rather than in
 * the page. Every other JS bridge in this tree passes a string OUT
 * (UTF8ToString on a C pointer); this one passes a variable-length string IN,
 * which needs a buffer in the module's heap — and the heap views and the
 * allocator are in scope here while being no part of what emscripten publishes
 * on Module. So the page gets a function that takes a JS string, and the
 * pointer discipline stays on this side of the boundary: allocated and freed
 * in the one call, with the heap view read after the allocation because
 * growing the heap replaces it.
 */
EM_JS(void, project_install_bridge, (void), {
	window.kruddRunProject = function (text) {
		var bytes = new TextEncoder().encode(String(text));
		var ptr = _malloc(bytes.length + 1);
		if (!ptr)
			return -1;
		HEAPU8.set(bytes, ptr);
		HEAPU8[ptr + bytes.length] = 0;
		var idx = _krudd_load_project(ptr);
		_free(ptr);
		return idx;
	};
})
#endif

void project_host_plugin_entry(struct subsystem_manager *mgr)
{
	/* The "scene" api is the entity plugin, registered before this one. */
	g_scene = subsystem_manager_get_api(mgr, "scene");
	subsystem_manager_register(mgr, &project_desc);
#ifdef __EMSCRIPTEN__
	project_install_bridge();
#endif
}
