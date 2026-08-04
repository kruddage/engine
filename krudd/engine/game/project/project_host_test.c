/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * project_host — a project driven end to end from a source string, with no C
 * plugin behind it. It boots the real s7 image, the real launcher registry and
 * a real world behind a stand-in "scene" api (the same three vtable entries the
 * entity plugin publishes: clear, build, dispatch), then evaluates a project
 * source and checks the four things the host promises.
 *
 * All of that bring-up is game/project_test's now — one link, one header and
 * one call, instead of the vtable and the four compiled-in engine sources this
 * file used to carry and every project copied off it (#1035). The harness is
 * exercised here first, by the module that owns the host it drives, before any
 * project is moved onto it. What is left below is what this test is actually
 * about: the source strings and the claims.
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
 * The last group is project_host_load — the door a project opened while the
 * engine is already running comes in by, which is one call rather than two so
 * that "open this file" has one meaning: it evaluates, then opens. What it adds
 * over project_host_eval is the replacement rule, and that is what those checks
 * are about: one launcher entry for whatever was opened last, renamed rather
 * than accumulated, with the previous project's world and hooks gone and the
 * entries the build itself registered untouched.
 *
 * No GPU and no asset catalog: the scene's mesh paths resolve to "unbound",
 * which still spawns every entity it declares, so the whole path is checkable
 * headless.
 */
#include <project_test/project_test.h>

#include <project/project_host.h>

#include <entity/world.h>
#include <host/game.h>
#include <log/log.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* The harness's world, borrowed for the life of the process. */
static struct world *w;

/* The launcher slot the project took, and the one a plain C game took. */
static int32_t g_project;
static int     g_other;

/* Bumped by the C game's own load callback — the gate's other side. */
static int     g_other_loads;

/*
 * A plain C game beside the project — the launcher entry a compiled-in plugin
 * registers, which is what the gate has to tell a project apart from. It builds
 * a scene of its own so that a selection made while it is loaded is a real one.
 */
static void other_load(void)
{
	world_reset(w);
	project_test_build("(scene other (entity (name \"other-pad\")))");
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

/*
 * Two projects standing in for files someone opened, each with a scene of one
 * named pad and a frame counter of its own — enough to tell whose world is
 * standing and whose hooks are running, which is the whole of what replacement
 * has to get right.
 */
static const char *DISK_A_SRC =
	"(project \"Disk One\"\n"
	"  (rules\n"
	"   (define *a-ticks* 0)\n"
	"   (define (a-frame) (set! *a-ticks* (+ *a-ticks* 1)))\n"
	"   (define (a-ticks ignored) *a-ticks*))\n"
	"  (scene disk-a (entity (name \"a-pad\")))\n"
	"  (on-tick a-frame))\n";

static const char *DISK_B_SRC =
	"(project \"Disk Two\"\n"
	"  (rules\n"
	"   (define *b-ticks* 0)\n"
	"   (define (b-frame) (set! *b-ticks* (+ *b-ticks* 1)))\n"
	"   (define (b-ticks ignored) *b-ticks*))\n"
	"  (scene disk-b (entity (name \"b-pad\")))\n"
	"  (on-tick b-frame))\n";

/* The launcher entry the two above share — the door's one slot. */
static int32_t g_door;

/*
 * Read one of the project's counters back out of the image. Every one of these
 * takes an argument it ignores, which is what the name says: a poll, not a
 * call.
 */
static int32_t poll(const char *fn)
{
	return project_test_call(fn, 0);
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
	assert(w->count == 0);
	assert(game_active_index() == -1);
	project_test_tick();
	assert(poll("demo-loads") == 0);
	assert(poll("demo-ticks") == 0);
}

/* Choosing it clears the world, builds the scene, then runs the reset hook. */
static void test_load_builds_scene(void)
{
	game_load(g_project);
	assert(game_active_index() == g_project);
	assert(w->count == 2);
	assert(project_test_entity("pad-a") >= 0);
	assert(project_test_entity("pad-b") >= 0);
	assert(poll("demo-loads") == 1);
	/* And it ran AFTER the build: the hook looked the first entity up by
	 * name from inside itself and found the scene already standing. */
	assert(poll("demo-built") == 1);
	assert(poll("demo-picks") == 0);
}

/* The frame hook runs once per tick while this is the loaded project. */
static void test_tick_hook(void)
{
	project_test_tick();
	assert(poll("demo-ticks") == 1);
	project_test_tick();
	project_test_tick();
	assert(poll("demo-ticks") == 3);
}

/*
 * The selection hook fires on the CHANGE, once per click: a held selection does
 * not fire again, a selection going away fires nothing but moves the baseline,
 * and re-selecting what was selected before that fires again.
 */
static void test_selection_edge(void)
{
	int32_t a = project_test_entity("pad-a");
	int32_t b = project_test_entity("pad-b");

	world_set_selected(w, a);
	project_test_tick();
	assert(poll("demo-picks") == 1);
	assert(poll("demo-last") == a);
	/* The hook ran with the world bound: it could outline what it was
	 * handed, which only a bound world can answer for. */
	assert(world_get_outline(w) == a);

	/* Held across frames: still one click. */
	project_test_tick();
	project_test_tick();
	assert(poll("demo-picks") == 1);

	/* A different entity is a second click. */
	world_set_selected(w, b);
	project_test_tick();
	assert(poll("demo-picks") == 2);
	assert(poll("demo-last") == b);

	/* Deselecting fires nothing — it is the release, not another click. */
	world_set_selected(w, -1);
	project_test_tick();
	assert(poll("demo-picks") == 2);

	/* And selecting the same entity again is an edge once more. */
	world_set_selected(w, b);
	project_test_tick();
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
	project_test_tick();
	project_test_tick();
	assert(poll("demo-ticks") == ticks);
	assert(poll("demo-picks") == picks);

	/* A click landing in the other game's scene reaches no project. */
	world_set_selected(w, 0);
	project_test_tick();
	assert(poll("demo-picks") == picks);

	/* Back to the project: its own load resets the counters, and the first
	 * frame after it does not report the selection it was away for. */
	game_load(g_project);
	assert(poll("demo-loads") == 2);
	project_test_tick();
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
	assert(w->count == 2);
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
	project_test_tick();
	assert(poll("demo-ticks") >= 1);

	/* And the rules clause that faulted still defined what it got through
	 * first: the refusal is about registration, not a transaction over the
	 * image. */
	assert(poll("demo-x") == 1);
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
	assert(w->count == 0);
	project_test_tick();
	assert(w->count == 0);

	/* And the project it displaced is untouched by any of it. */
	game_load(g_project);
	assert(w->count == 2);
}

/*
 * A scene bigger than the printer's default list elision, built whole.
 *
 * The scene clause is data the host writes back out as text for scene-build!,
 * and s7's printer replaces everything past (*s7* 'print-length) items — 12 by
 * default — with "...". A real project's scene is a hundred entities before it
 * is anything, so without project-scene-source raising that for the span of the
 * write, a project would load its first dozen entities and report success: a
 * board with two rows of squares and nothing on the log. Forty is comfortably
 * past the default and small enough to stay a cheap check.
 */
static void test_large_scene_is_not_elided(void)
{
	static char src[4096];
	int32_t     big;
	size_t      n;
	int         i;

	n = (size_t)snprintf(src, sizeof(src), "(project \"Wide\" (scene wide");
	for (i = 0; i < 40; i++)
		n += (size_t)snprintf(src + n, sizeof(src) - n,
				      " (entity (name \"pad-%d\") (at %d 0 0))",
				      i, i);
	snprintf(src + n, sizeof(src) - n, "))");

	big = project_host_eval(src);
	assert(big >= 0);
	game_load(big);
	assert(w->count == 40);
	assert(project_test_entity("pad-0") >= 0);
	assert(project_test_entity("pad-39") >= 0);

	/* And the project it displaced still loads its own scene whole. */
	game_load(g_project);
	assert(w->count == 2);
}

/*
 * project_host_load is evaluate-and-open in one call: the source registers, and
 * the project it registered is the loaded one before the call returns — the
 * world it displaced gone, its own scene standing, its hooks live.
 */
static void test_load_opens_it(void)
{
	int32_t demo_ticks = poll("demo-ticks");
	int     games      = game_count();

	g_door = project_host_load(DISK_A_SRC);
	assert(g_door >= 0 && g_door != g_project);
	assert(game_count() == games + 1);
	assert(game_active_index() == g_door);

	/* Its scene, and only its scene. */
	assert(w->count == 1);
	assert(project_test_entity("a-pad") >= 0);
	assert(project_test_entity("pad-a") < 0);

	/* Its hooks run; the project it displaced has stopped. */
	project_test_tick();
	assert(poll("a-ticks") == 1);
	assert(poll("demo-ticks") == demo_ticks);
}

/*
 * A second file replaces the first rather than layering over it: the same
 * launcher entry, renamed, and no trace of the project that was there. This is
 * the check the door exists for — five files opened must leave one button, not
 * five, and the first one's hooks must not still be firing behind the fifth.
 */
static void test_load_replaces_the_previous(void)
{
	int32_t a_ticks = poll("a-ticks");
	int     games   = game_count();

	assert(project_host_load(DISK_B_SRC) == g_door);
	assert(game_count() == games);
	assert(game_find("Disk One") == -1);
	assert(game_find("Disk Two") == g_door);

	assert(w->count == 1);
	assert(project_test_entity("b-pad") >= 0);
	assert(project_test_entity("a-pad") < 0);

	project_test_tick();
	assert(poll("b-ticks") == 1);
	assert(poll("a-ticks") == a_ticks);
}

/*
 * A file whose project is already on the launcher is a reload of that entry,
 * not a copy of it under the door's — which is the path opening the staged
 * project's own source out of assets/ takes. The door's entry is left standing:
 * it holds the last project opened THROUGH it, and this was not one.
 */
static void test_load_reuses_a_registered_name(void)
{
	int games = game_count();

	assert(project_host_load(PROJECT_SRC) == g_project);
	assert(game_count() == games);
	assert(game_find("Disk Two") == g_door);
	assert(game_active_index() == g_project);
	assert(w->count == 2);
	assert(poll("demo-loads") == 1);
}

/*
 * A file that is not a project leaves the engine exactly as it was: nothing
 * opened, nothing registered, and the project already loaded still running its
 * own frames. A bad .scm is the expected case for a button that opens whatever
 * it is pointed at.
 */
static void test_load_of_a_bad_source_changes_nothing(void)
{
	int32_t ticks = poll("demo-ticks");
	int     games = game_count();

	assert(project_host_load("(project \"Unclosed\"") == -1);
	assert(project_host_load("(define not-a-project 1)") == -1);
	assert(project_host_load("(project 42 (on-tick demo-frame))") == -1);
	assert(game_count() == games);
	assert(game_find("Disk Two") == g_door);

	assert(game_active_index() == g_project);
	assert(w->count == 2);
	project_test_tick();
	assert(poll("demo-ticks") == ticks + 1);
}

int main(void)
{
	/*
	 * The LEAN half of the harness: no asset catalog, because a project's
	 * mesh and material paths resolving to "unbound" still spawns every
	 * entity its scene declares, and what is under test here is the host
	 * rather than what its scenes bind to.
	 */
	project_test_init();
	w = project_test_world();

	test_registers();
	test_load_builds_scene();
	test_tick_hook();
	test_selection_edge();
	test_hooks_gated_on_active();
	test_reload_keeps_its_slot();
	test_name_collision_refused();
	test_malformed_is_refused();
	test_partial_project_loads();
	test_large_scene_is_not_elided();
	test_load_opens_it();
	test_load_replaces_the_previous();
	test_load_reuses_a_registered_name();
	test_load_of_a_bad_source_changes_nothing();

	printf("project_host_test: ok\n");
	return 0;
}
