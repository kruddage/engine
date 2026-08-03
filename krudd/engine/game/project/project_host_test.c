/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * project_host — a project driven end to end from a source string, with no C
 * plugin behind it. It boots the real s7 image, the real launcher registry and
 * a real world behind a stand-in "scene" api (the same three vtable entries the
 * entity plugin publishes: clear, build, dispatch), then evaluates a project
 * source and checks the four things the host promises.
 *
 *   - a (project ...) form puts an entry on the launcher, and choosing it
 *     clears the world, builds the project's scene and runs its reset hook;
 *   - while it is the loaded project its frame hook runs each tick, and its
 *     selection hook fires once per CHANGE of selection — not once per frame a
 *     selection is held, and not for a selection going away;
 *   - none of that fires while some other game is the loaded one, and a
 *     selection made in the meantime is not delivered late when the project
 *     comes back;
 *   - a malformed or partial form is refused with a log line and registers
 *     nothing, since a project is user input rather than a compiled-in plugin.
 *
 * No GPU and no asset catalog: the scene's mesh paths resolve to "unbound",
 * which still spawns every entity it declares, so the whole path is checkable
 * headless.
 */
#include <project/project_host.h>

#include <entity/world.h>
#include <entity/scene_script.h>
#include <abi/entity_api.h>
#include <core/subsystem_manager.h>
#include <host/game.h>
#include <log/log.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* One world instance reused across the checks; too big for the stack. */
static struct world w;

static struct subsystem_manager mgr;

/* The launcher slot the project took, and the one a plain C game took. */
static int32_t g_project;
static int     g_other;

/* Bumped by the C game's own load callback — the gate's other side. */
static int     g_other_loads;

/*
 * The stand-in "scene" subsystem: the three entity_api entries a project's host
 * actually uses, each binding this test's world exactly as entity_plugin.c
 * binds the live one. There is deliberately no get_selected among them — the
 * host reads the selection from inside the image, through the same
 * (scene-selected) primitive a project would, and never through the vtable.
 */
static int32_t t_build_scene_scm(const char *src)
{
	return scene_script_build(&w, NULL, src);
}

static int32_t t_dispatch_scm(const char *fn, int32_t arg)
{
	return scene_script_call(&w, NULL, fn, arg);
}

static void t_clear_world(void)
{
	world_reset(&w);
}

static const struct entity_api t_scene_api = {
	.build_scene_scm = t_build_scene_scm,
	.dispatch_scm    = t_dispatch_scm,
	.clear_world     = t_clear_world,
};

static void t_scene_init(void)
{
	world_reset(&w);
	/* What entity_plugin.c's scene_init does: the scene-* primitives a
	 * (scene ...) form and a project's load path are written in. */
	scene_script_init();
}

static const struct subsystem t_subsystems[] = {
	{ .name = "scene", .api = &t_scene_api, .init = t_scene_init },
	{ NULL }
};

/*
 * A plain C game beside the project — the launcher entry a compiled-in plugin
 * registers, which is what the gate has to tell a project apart from. It builds
 * a scene of its own so that a selection made while it is loaded is a real one.
 */
static void other_load(void)
{
	world_reset(&w);
	scene_script_build(&w, NULL,
			   "(scene other (entity (name \"other-pad\")))");
	g_other_loads++;
}

/*
 * The project under test. Its rules keep four counters and expose each through
 * a one-argument poll procedure, which is how a C test reads image state back:
 * dispatch_scm calls a named procedure with one integer and hands back its
 * integer result.
 *
 * Nothing here is engine vocabulary except the primitives — scene-outline! is
 * called for the same reason a real game would, to prove a hook runs with the
 * world bound and not merely that it runs.
 */
static const char *PROJECT_SRC =
	"(project \"Test Project\"\n"
	"  (rules\n"
	"   (define *demo-loads* 0)\n"
	"   (define *demo-ticks* 0)\n"
	"   (define *demo-picks* 0)\n"
	"   (define *demo-last* -1)\n"
	"   (define *demo-built* 0)\n"
	"   (define (demo-reset . ignored)\n"
	"     (set! *demo-loads* (+ *demo-loads* 1))\n"
	"     (set! *demo-ticks* 0)\n"
	"     (set! *demo-picks* 0)\n"
	"     (set! *demo-last* -1)\n"
	"     (set! *demo-built*\n"
	"           (if (string=? (scene-entity-name 0) \"pad-a\") 1 0)))\n"
	"   (define (demo-frame)\n"
	"     (set! *demo-ticks* (+ *demo-ticks* 1)))\n"
	"   (define (demo-picked id)\n"
	"     (set! *demo-picks* (+ *demo-picks* 1))\n"
	"     (set! *demo-last* id)\n"
	"     (scene-outline! id))\n"
	"   (define (demo-loads ignored) *demo-loads*)\n"
	"   (define (demo-built ignored) *demo-built*)\n"
	"   (define (demo-ticks ignored) *demo-ticks*)\n"
	"   (define (demo-picks ignored) *demo-picks*)\n"
	"   (define (demo-last ignored) *demo-last*))\n"
	"  (scene demo\n"
	"         (entity (name \"pad-a\") (mesh \"builtin://mesh/plane\")\n"
	"                 (at -1 0 0))\n"
	"         (entity (name \"pad-b\") (mesh \"builtin://mesh/plane\")\n"
	"                 (at 1 0 0)))\n"
	"  (on-load demo-reset)\n"
	"  (on-tick demo-frame)\n"
	"  (on-selected demo-picked))\n";

/* Read one of the project's counters back out of the image. */
static int32_t poll(const char *fn)
{
	return scene_script_call(&w, NULL, fn, 0);
}

/* The entity id of the named entity, or -1 — the "what was clicked" a real
 * picker would hand the engine. */
static int32_t id_of(const char *name)
{
	uint32_t e;

	for (e = 0; e < w.count; e++) {
		const char *n = w.alive[e] ? world_entity_name(&w, e) : NULL;

		if (n && strcmp(n, name) == 0)
			return (int32_t)e;
	}
	return -1;
}

/* True when a warning mentioning TEXT is on the log. */
static int logged_warning(const char *text)
{
	static struct log_message hist[LOG_HISTORY_CAP];
	uint32_t                  n = log_get_history(hist, LOG_HISTORY_CAP);
	uint32_t                  i;

	for (i = 0; i < n; i++) {
		if (hist[i].level >= LOG_LEVEL_WARN && strstr(hist[i].text, text))
			return 1;
	}
	return 0;
}

/*
 * Evaluate SRC expecting a refusal: -1, TEXT on the log, and nothing added to
 * the launcher. The history is cleared first so the line found is this call's
 * own rather than one an earlier check left behind.
 */
static void expect_refusal(const char *src, const char *text)
{
	int games = game_count();

	log_init();
	assert(project_host_eval(src) == -1);
	assert(logged_warning(text));
	assert(game_count() == games);
}

/*
 * A project source with no C behind it registers a launcher entry, and only
 * that: nothing is built and no hook runs until the launcher picks it.
 */
static void test_registers(void)
{
	g_project = project_host_eval(PROJECT_SRC);
	assert(g_project >= 0);
	assert(game_count() == 1);
	assert(game_find("Test Project") == g_project);

	/* Registered but not loaded: the world is untouched and, with no active
	 * game at all, a frame runs none of its hooks. */
	assert(w.count == 0);
	assert(game_active_index() == -1);
	subsystem_manager_tick(&mgr);
	assert(poll("demo-loads") == 0);
	assert(poll("demo-ticks") == 0);
}

/* Choosing it clears the world, builds the scene, then runs the reset hook. */
static void test_load_builds_scene(void)
{
	game_load(g_project);
	assert(game_active_index() == g_project);
	assert(w.count == 2);
	assert(id_of("pad-a") >= 0 && id_of("pad-b") >= 0);
	assert(poll("demo-loads") == 1);
	/* And it ran AFTER the build: the hook looked the first entity up by
	 * name from inside itself and found the scene already standing. */
	assert(poll("demo-built") == 1);
	assert(poll("demo-picks") == 0);
}

/* The frame hook runs once per tick while this is the loaded project. */
static void test_tick_hook(void)
{
	subsystem_manager_tick(&mgr);
	assert(poll("demo-ticks") == 1);
	subsystem_manager_tick(&mgr);
	subsystem_manager_tick(&mgr);
	assert(poll("demo-ticks") == 3);
}

/*
 * The selection hook fires on the CHANGE, once per click: a held selection does
 * not fire again, a selection going away fires nothing but moves the baseline,
 * and re-selecting what was selected before that fires again.
 */
static void test_selection_edge(void)
{
	int32_t a = id_of("pad-a");
	int32_t b = id_of("pad-b");

	world_set_selected(&w, a);
	subsystem_manager_tick(&mgr);
	assert(poll("demo-picks") == 1);
	assert(poll("demo-last") == a);
	/* The hook ran with the world bound: it could outline what it was
	 * handed, which only a bound world can answer for. */
	assert(world_get_outline(&w) == a);

	/* Held across frames: still one click. */
	subsystem_manager_tick(&mgr);
	subsystem_manager_tick(&mgr);
	assert(poll("demo-picks") == 1);

	/* A different entity is a second click. */
	world_set_selected(&w, b);
	subsystem_manager_tick(&mgr);
	assert(poll("demo-picks") == 2);
	assert(poll("demo-last") == b);

	/* Deselecting fires nothing — it is the release, not another click. */
	world_set_selected(&w, -1);
	subsystem_manager_tick(&mgr);
	assert(poll("demo-picks") == 2);

	/* And selecting the same entity again is an edge once more. */
	world_set_selected(&w, b);
	subsystem_manager_tick(&mgr);
	assert(poll("demo-picks") == 3);
}

/*
 * Nothing fires while some other game is the loaded one — the host makes the
 * check no project has to remember. A selection made while away is not
 * delivered late when the project comes back either: its load re-arms the
 * baseline from the world it just built.
 */
static void test_hooks_gated_on_active(void)
{
	int32_t ticks = poll("demo-ticks");
	int32_t picks = poll("demo-picks");

	g_other = game_register("Other", other_load);
	assert(g_other >= 0);
	game_load(g_other);
	assert(g_other_loads == 1);

	/* Its load emptied the world; a project's hooks are silent throughout. */
	subsystem_manager_tick(&mgr);
	subsystem_manager_tick(&mgr);
	assert(poll("demo-ticks") == ticks);
	assert(poll("demo-picks") == picks);

	/* A click landing in the other game's scene reaches no project. */
	world_set_selected(&w, 0);
	subsystem_manager_tick(&mgr);
	assert(poll("demo-picks") == picks);

	/* Back to the project: its own load resets the counters, and the first
	 * frame after it does not report the selection it was away for. */
	game_load(g_project);
	assert(poll("demo-loads") == 2);
	subsystem_manager_tick(&mgr);
	assert(poll("demo-picks") == 0);
	assert(poll("demo-ticks") == 1);
}

/*
 * Re-evaluating a project's source is how a project reloads: it keeps the
 * launcher slot it had rather than stacking a second entry beside it, and the
 * slot still loads the project (the newly registered thunk, not a stale one).
 */
static void test_reload_keeps_its_slot(void)
{
	int32_t again = project_host_eval(PROJECT_SRC);

	assert(again == g_project);
	assert(game_count() == 2);
	game_load(g_project);
	assert(w.count == 2);
	assert(poll("demo-loads") == 1);   /* the rules were re-evaluated too */
}

/*
 * A name another game already holds is refused: that entry's load callback
 * belongs to something else and cannot be replaced, so accepting would leave a
 * project that never loads.
 */
static void test_name_collision_refused(void)
{
	expect_refusal("(project \"Other\"\n"
		       "  (scene x (entity (name \"n\"))))\n",
		       "already on the launcher");

	/* The entry that was there still runs its own C load callback. */
	game_load(g_other);
	assert(g_other_loads == 2);
	game_load(g_project);
}

/*
 * Every way a project form can be wrong: refused with a log line, registering
 * nothing, never a fault. A bad project is user input, not a bug.
 */
static void test_malformed_is_refused(void)
{
	/* Text that does not read at all. */
	expect_refusal("(project \"Unclosed\"", "source fault");

	/* Text that reads, but declares no project. */
	expect_refusal("(define demo-nothing 1)",
		       "declared no (project ...) form");

	/* A form with no display name, or one that is not a string. */
	expect_refusal("(project)", "opens with its display name");
	expect_refusal("(project unnamed (on-tick demo-frame))",
		       "opens with its display name");

	/* A (rules ...) clause that faults partway through. */
	expect_refusal("(project \"Bad Rules\"\n"
		       "  (rules (define (demo-x ignored) 1)\n"
		       "         (car '())))",
		       "(rules ...) faulted");

	/* Hooks that name nothing, name a non-procedure, or name nothing at
	 * all: a hook that silently never runs is worth refusing over, since a
	 * project whose clicks do nothing looks exactly like a broken engine. */
	expect_refusal("(project \"No Hook\" (on-selected demo-nonexistent))",
		       "(on-selected ...) names no procedure");
	expect_refusal("(project \"Not A Proc\" (on-tick 42))",
		       "(on-tick ...) names no procedure");
	expect_refusal("(project \"Empty\" (on-load))",
		       "empty (on-load ...) clause");

	/* The loaded project came through all of it still running. */
	subsystem_manager_tick(&mgr);
	assert(poll("demo-ticks") >= 1);

	/* And the rules clause that faulted still defined what it got through
	 * first: the refusal is about registration, not a transaction over the
	 * image. */
	assert(scene_script_call(&w, NULL, "demo-x", 0) == 1);
}

/*
 * A project may be partial: a name and nothing else is a legal project. It
 * registers, and choosing it empties the world and runs no hooks — which is
 * what "no clauses" means, rather than a crash or a stale scene.
 */
static void test_partial_project_loads(void)
{
	int32_t bare = project_host_eval("(project \"Bare\")");

	assert(bare >= 0 && bare != g_project);
	assert(game_count() == 3);
	game_load(bare);
	assert(game_active_index() == bare);
	assert(w.count == 0);
	subsystem_manager_tick(&mgr);
	assert(w.count == 0);

	/* And the project it displaced is untouched by any of it. */
	game_load(g_project);
	assert(w.count == 2);
}

int main(void)
{
	log_init();
	subsystem_manager_init(&mgr, t_subsystems);
	project_host_plugin_entry(&mgr);

	test_registers();
	test_load_builds_scene();
	test_tick_hook();
	test_selection_edge();
	test_hooks_gated_on_active();
	test_reload_keeps_its_slot();
	test_name_collision_refused();
	test_malformed_is_refused();
	test_partial_project_loads();

	printf("project_host_test: ok\n");
	return 0;
}
