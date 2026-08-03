/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * scene_script — the scene-building path end to end, minus the GPU. It boots the
 * real s7 image (which loads the embedded scene_script.scm), registers the
 * scene-* host primitives, and builds a (scene ...) form against a stand-in asset
 * catalog — then checks each spawned entity carries the transform, name, and
 * mesh/material/script bindings the form declared, and that the resolver turned
 * catalog paths into the right stable ids.
 *
 * It also covers the two runtime seams onto that world: an event dispatch
 * (scene_script_call) and the frame hook (scene_script_tick), including that
 * both leave the world unbound behind them.
 */
#include <entity/world.h>
#include <entity/scene.h>
#include <abi/asset_api.h>
#include <entity/scene_script.h>

#include <core/script.h>
#include <log/log.h>

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* One world instance reused across the checks; too big for the stack. */
static struct world w;

static int feq(float a, float b)
{
	float d = a - b;

	return (d < 0.0f ? -d : d) < 1e-4f;
}

/*
 * A stand-in catalog: six built-ins the scenes below reference, each at a stable
 * id the checks assert against. Only count/info are exercised — scene building
 * resolves paths to ids and never reads asset bytes.
 */
static const struct {
	const char *path;
	uint32_t    id;
	int32_t     type;
} g_catalog[] = {
	{ "builtin://mesh/plane",         10, ASSET_TYPE_MESH     },
	{ "builtin://mesh/torus",         11, ASSET_TYPE_MESH     },
	{ "builtin://mesh/box",           12, ASSET_TYPE_MESH     },
	{ "builtin://material/checker",   20, ASSET_TYPE_MATERIAL },
	{ "builtin://material/pbr-metal", 21, ASSET_TYPE_MATERIAL },
	{ "builtin://script/spinner",     30, ASSET_TYPE_SCRIPT   },
};

static uint32_t fake_count(void)
{
	return (uint32_t)(sizeof(g_catalog) / sizeof(g_catalog[0]));
}

static int32_t fake_info(uint32_t i, struct asset_info *out)
{
	if (!out || i >= fake_count())
		return -1;
	memset(out, 0, sizeof(*out));
	out->path = g_catalog[i].path;
	out->id   = g_catalog[i].id;
	out->type = g_catalog[i].type;
	return 0;
}

static const struct asset_api fake_asset = {
	.count = fake_count,
	.info  = fake_info,
};

/* The board + two marks scene, exercising every clause kind. */
static const char *SCENE_SRC =
	"(scene tic-tac-toe"
	"  (entity (name \"board\")"
	"          (mesh \"builtin://mesh/plane\")"
	"          (material \"builtin://material/checker\")"
	"          (at 0 0 0) (scale 3 3 3))"
	"  (entity (name \"o-a1\")"
	"          (mesh \"builtin://mesh/torus\")"
	"          (material \"builtin://material/pbr-metal\")"
	"          (at -1 0.15 -1) (scale 0.4 0.4 0.4))"
	"  (entity (name \"spin\")"
	"          (mesh \"builtin://mesh/box\")"
	"          (script \"builtin://script/spinner\")"
	"          (rotate 0 90 0)))";

/* A full build binds every declared asset, name, and transform. */
static void test_build_binds_everything(void)
{
	int32_t n;

	world_reset(&w);
	n = scene_script_build(&w, &fake_asset, SCENE_SRC);
	assert(n == 3);

	/* Entity 0: the board — mesh + material + name + position + scale. */
	assert(w.alive[0]);
	assert(w.mask[0] & COMPONENT_RENDER);
	assert(w.mask[0] & COMPONENT_MATERIAL);
	assert(w.mask[0] & COMPONENT_NAME);
	assert(w.render_ref[0] == 10);
	assert(w.material_ref[0] == 20);
	assert(strcmp(world_entity_name(&w, 0), "board") == 0);
	assert(feq(w.local[0].position[0], 0.0f));
	assert(feq(w.local[0].scale[0], 3.0f));
	assert(feq(w.local[0].scale[1], 3.0f));
	assert(feq(w.local[0].scale[2], 3.0f));

	/* Entity 1: an O — torus mesh, metal material, its own cell + scale. */
	assert(w.render_ref[1] == 11);
	assert(w.material_ref[1] == 21);
	assert(feq(w.local[1].position[0], -1.0f));
	assert(feq(w.local[1].position[1], 0.15f));
	assert(feq(w.local[1].position[2], -1.0f));
	assert(feq(w.local[1].scale[0], 0.4f));

	/* Entity 2: an X — box mesh + a bound script, no material. */
	assert(w.render_ref[2] == 12);
	assert(w.mask[2] & COMPONENT_SCRIPT);
	assert(w.script_ref[2] == 30);
	assert(!(w.mask[2] & COMPONENT_MATERIAL));
}

/*
 * (rotate 0 90 0) is a quarter turn about +Y: the authored quaternion is
 * (0, sin45, 0, cos45). The build writes the AUTHORED pose (local), leaving
 * world_xform for the tick to derive — so we read local here.
 */
static void test_rotate_builds_quaternion(void)
{
	float half = (float)(M_PI / 4.0);

	world_reset(&w);
	assert(scene_script_build(&w, &fake_asset, SCENE_SRC) == 3);
	assert(feq(w.local[2].rotation[0], 0.0f));
	assert(feq(w.local[2].rotation[1], sinf(half)));
	assert(feq(w.local[2].rotation[2], 0.0f));
	assert(feq(w.local[2].rotation[3], cosf(half)));
}

/* An unresolved path binds nothing (ref 0) and never faults the build. */
static void test_unknown_path_is_inert(void)
{
	int32_t n;

	world_reset(&w);
	n = scene_script_build(&w, &fake_asset,
			       "(scene s (entity (mesh \"builtin://mesh/nope\")))");
	assert(n == 1);
	assert(w.alive[0]);
	assert(w.render_ref[0] == 0);
	assert(!(w.mask[0] & COMPONENT_RENDER));
}

/*
 * A (children ...) clause nests entities under their declaring parent: the count
 * covers the whole subtree, each child records its parent, and a mesh-less parent
 * (the X group) carries a name but no render component.
 */
static void test_children_nest_under_parent(void)
{
	int32_t n;

	world_reset(&w);
	n = scene_script_build(&w, &fake_asset,
			       "(scene s"
			       "  (entity (name \"x\") (at 0 0.15 0)"
			       "          (children"
			       "            (entity (mesh \"builtin://mesh/box\")"
			       "                    (rotate 0 45 0))"
			       "            (entity (mesh \"builtin://mesh/box\")"
			       "                    (rotate 0 -45 0)))))");
	assert(n == 3);               /* the parent plus its two bars */
	assert(w.count == 3);

	/* Entity 0 is the group: named, positioned, but nothing to draw. */
	assert(strcmp(world_entity_name(&w, 0), "x") == 0);
	assert(!(w.mask[0] & COMPONENT_RENDER));
	assert(feq(w.local[0].position[1], 0.15f));

	/* Entities 1 and 2 are the bars, each parented to the group. */
	assert(w.parent[1] == 0);
	assert(w.parent[2] == 0);
	assert(w.render_ref[1] == 12);
	assert(w.render_ref[2] == 12);
}

/*
 * The runtime seam: scene_script_call invokes a named image fn with the world
 * bound (so scene-* primitives work) and returns its int; scene-entity-name reads
 * a live entity's name. This is what a game's event rules run on.
 */
static void test_dispatch_and_entity_name(void)
{
	int32_t r;

	world_reset(&w);
	assert(scene_script_build(&w, &fake_asset,
		"(scene s (entity (name \"alpha\")) (entity (name \"beta\")))") == 2);

	/* A one-off rule: spawn a child of the entity iff its name reads "beta" —
	 * proving the world is bound (scene-spawn lands) and the name resolves. */
	script_eval("(define (probe id)"
		    "  (if (string=? (scene-entity-name id) \"beta\")"
		    "      (begin (scene-spawn id) 1) 0))");

	r = scene_script_call(&w, &fake_asset, "probe", 1);   /* id 1 is beta */
	assert(r == 1);
	assert(w.count == 3);            /* the child spawned */
	assert(w.parent[2] == 1);       /* parented to beta */

	r = scene_script_call(&w, &fake_asset, "probe", 0);   /* id 0 is alpha */
	assert(r == 0);
	assert(w.count == 3);            /* no spawn on the non-match */
}

/*
 * scene-destroy-named! tombstones every entity of a given name and cascades to
 * their children, leaving other names untouched — the sweep a game uses to clear
 * the board. Returns the match count (named roots, not the cascaded children).
 */
static void test_destroy_named(void)
{
	int32_t r;

	world_reset(&w);
	assert(scene_script_build(&w, &fake_asset,
		"(scene s"
		"  (entity (name \"mark\") (children (entity (name \"bar\"))))"
		"  (entity (name \"keep\"))"
		"  (entity (name \"mark\")))") == 4);
	/* ids: 0 mark(parent), 1 bar(child of 0), 2 keep, 3 mark */

	script_eval("(define (clear ignore) (scene-destroy-named! \"mark\"))");
	r = scene_script_call(&w, &fake_asset, "clear", 0);

	assert(r == 2);            /* two entities named "mark" matched */
	assert(!w.alive[0]);       /* the mark parent is gone */
	assert(!w.alive[1]);       /* its child cascaded away */
	assert(w.alive[2]);        /* an unrelated name survives */
	assert(!w.alive[3]);       /* the second mark is gone */
}

/*
 * The per-frame seam: scene_script_tick calls the image's (tick) with the world
 * bound, so a frame hook living in Scheme sees the live world — reads a name,
 * reads the selection, and writes the outline, none of which it could do while
 * (tick) was invoked unbound from core.
 */
static void test_tick_sees_the_world(void)
{
	world_reset(&w);
	assert(scene_script_build(&w, &fake_asset,
				  "(scene s (entity (name \"alpha\"))"
				  "         (entity (name \"beta\")))") == 2);
	world_set_selected(&w, 1);

	script_eval("(define *ticks* 0)"
		    "(define *seen-name* \"\")"
		    "(define (tick)"
		    "  (set! *ticks* (+ *ticks* 1))"
		    "  (set! *seen-name* (scene-entity-name (scene-selected)))"
		    "  (scene-outline! (scene-selected)))");

	scene_script_tick(&w, &fake_asset);

	/* The outline write landed on the live world. */
	assert(world_get_outline(&w) == 1);

	/* And the read side saw the selected entity's real name. */
	script_eval("(define (seen ignore)"
		    "  (if (string=? *seen-name* \"beta\") *ticks* -1))");
	assert(scene_script_call(&w, &fake_asset, "seen", 0) == 1);

	scene_script_tick(&w, &fake_asset);
	assert(scene_script_call(&w, &fake_asset, "seen", 0) == 2);
}

/*
 * (scene-selected) reports exactly what world_get_selected does — the value
 * entity_api's get_selected hands the rest of the engine — and -1 when nothing
 * is selected.
 */
static void test_selected_tracks_the_world(void)
{
	world_reset(&w);
	assert(scene_script_build(&w, &fake_asset,
				  "(scene s (entity (name \"alpha\"))"
				  "         (entity (name \"beta\")))") == 2);
	script_eval("(define (sel ignore) (scene-selected))");

	world_set_selected(&w, 1);
	assert(world_get_selected(&w) == 1);
	assert(scene_script_call(&w, &fake_asset, "sel", 0) == 1);

	world_set_selected(&w, 0);
	assert(scene_script_call(&w, &fake_asset, "sel", 0) ==
	       world_get_selected(&w));

	world_set_selected(&w, -1);
	assert(world_get_selected(&w) == -1);
	assert(scene_script_call(&w, &fake_asset, "sel", 0) == -1);
}

/*
 * The world is unbound the moment the tick returns: a scene-* primitive
 * evaluated at top level afterwards reads an empty world and mutates nothing.
 * script_eval runs outside any bound call, so these three lines are exactly the
 * "no primitive can observe a stale world outside a tick" case; a later bound
 * call only reports what they captured.
 */
static void test_world_unbound_after_tick(void)
{
	uint32_t before;

	world_reset(&w);
	assert(scene_script_build(&w, &fake_asset,
		"(scene s (entity (name \"alpha\")))") == 1);
	world_set_selected(&w, 0);
	script_eval("(define (tick) (scene-outline! 0))");
	scene_script_tick(&w, &fake_asset);
	assert(world_get_outline(&w) == 0);

	before = w.count;
	script_eval("(define *out-sel* (scene-selected))"
		    "(define *out-name* (scene-entity-name 0))"
		    "(define *out-pos* (scene-entity-pos 0))"
		    "(define *out-spawn* (scene-spawn))");

	/* Nothing spawned into the world the tick left behind. */
	assert(w.count == before);
	assert(world_get_outline(&w) == 0);

	script_eval("(define (probe-unbound k)"
		    "  (cond ((= k 0) *out-sel*)"
		    "        ((= k 1) (string-length *out-name*))"
		    "        ((= k 2) (if *out-pos* 1 0))"
		    "        (else *out-spawn*)))");
	assert(scene_script_call(&w, &fake_asset, "probe-unbound", 0) == -1);
	assert(scene_script_call(&w, &fake_asset, "probe-unbound", 1) == 0);
	assert(scene_script_call(&w, &fake_asset, "probe-unbound", 2) == 0);
	assert(scene_script_call(&w, &fake_asset, "probe-unbound", 3) == -1);
}

/* An image with no (tick) is a no-op, and so is a tick with no world. */
static void test_tick_without_hook_is_inert(void)
{
	world_reset(&w);
	assert(scene_script_build(&w, &fake_asset,
		"(scene s (entity (name \"alpha\")))") == 1);
	script_eval("(define tick 0)");        /* not a procedure */
	scene_script_tick(&w, &fake_asset);
	assert(w.count == 1);
	scene_script_tick(NULL, &fake_asset);  /* no world — must not crash */
	assert(w.count == 1);
}

/* A non-scene form is rejected cleanly: nothing spawns, no crash. */
static void test_not_a_scene_form(void)
{
	world_reset(&w);
	assert(scene_script_build(&w, &fake_asset, "(mesh box)") == 0);
	assert(w.count == 0);
}

/* A per-entity fault is isolated: the good entities on either side still land. */
static void test_entity_fault_is_isolated(void)
{
	int32_t n;

	world_reset(&w);
	/* The middle entity's (name) clause has no argument, so reading its value
	 * throws — the per-entity catch drops just that one. It was already
	 * spawned (an empty entity 1) before the fault, so the surviving marks are
	 * entities 0 and 2. */
	n = scene_script_build(&w, &fake_asset,
			       "(scene s"
			       "  (entity (mesh \"builtin://mesh/box\"))"
			       "  (entity (name))"
			       "  (entity (mesh \"builtin://mesh/torus\")))");
	assert(n == 2);
	assert(w.render_ref[0] == 12);
	assert(!(w.mask[1] & COMPONENT_RENDER));
	assert(w.render_ref[2] == 11);
}

/*
 * (scene-clear!) is entity_api.clear_world reached from Scheme: the world
 * empties, and both the editor selection and the game outline go back to -1.
 */
static void test_scene_clear(void)
{
	world_reset(&w);
	assert(scene_script_build(&w, &fake_asset, SCENE_SRC) == 3);
	w.selected = 1;
	w.outline  = 2;

	script_eval("(define (wipe ignore) (scene-clear!) 7)");
	assert(scene_script_call(&w, &fake_asset, "wipe", 0) == 7);

	assert(w.count == 0);
	assert(w.selected == -1);
	assert(w.outline == -1);
}

/* (scene-build! src) builds into the bound world and returns its count. */
static void test_scene_build_counts(void)
{
	world_reset(&w);
	script_eval("(define (load-it ignore)"
		    "  (scene-build! \"(scene s (entity (name \\\"a\\\"))"
		    "                          (entity (name \\\"b\\\")))\"))");
	assert(scene_script_call(&w, &fake_asset, "load-it", 0) == 2);
	assert(w.count == 2);
	assert(strcmp(world_entity_name(&w, 0), "a") == 0);
	assert(strcmp(world_entity_name(&w, 1), "b") == 0);
}

/*
 * -1 on every way of not building: a form that is not a (scene ...), text that
 * does not read at all, an argument that is not a string, and a call made with
 * no world bound (script_eval runs outside any scene_script_call, which is also
 * the check that the outermost call really did unbind on its way out).
 */
static void test_scene_build_rejects(void)
{
	world_reset(&w);
	script_eval("(define *r* 0)");
	script_eval("(define (try src) (scene-build! src))");

	/* An integer where the source should be: nothing to read, so -1. */
	assert(scene_script_call(&w, &fake_asset, "try", 0) == -1);
	assert(w.count == 0);

	script_eval("(define (try-mesh ignore) (scene-build! \"(mesh box)\"))");
	assert(scene_script_call(&w, &fake_asset, "try-mesh", 0) == -1);

	script_eval("(define (try-junk ignore) (scene-build! \"(scene\"))");
	assert(scene_script_call(&w, &fake_asset, "try-junk", 0) == -1);

	/* No world bound out here: the primitive has nothing to build into. */
	script_eval("(set! *r* (scene-build! \"(scene s (entity))\"))");
	script_eval("(define (peek ignore) *r*)");
	assert(scene_script_call(&w, &fake_asset, "peek", 0) == -1);
	assert(w.count == 0);
}

/*
 * The re-entrancy check, and the reason the binding is saved and restored
 * rather than set and cleared. load-level runs inside a dispatch (world bound),
 * calls scene-build! (which binds the same world a second level down), and then
 * keeps working with the world it started with. Under set/clear the inner
 * build's exit nulls g_w, the trailing scene-spawn returns -1, and this goes
 * red — which is why the assert is on work done after the nested build.
 */
static void test_nested_build_keeps_outer_binding(void)
{
	int32_t r;

	world_reset(&w);
	script_eval("(define (load-level ignore)"
		    "  (scene-clear!)"
		    "  (let ((n (scene-build!"
		    "             \"(scene lv (entity (name \\\"a\\\"))"
		    "                       (entity (name \\\"b\\\")))\")))"
		    "    (+ (* 100 n) (scene-spawn -1))))");

	r = scene_script_call(&w, &fake_asset, "load-level", 0);

	/* 2 built, then a third spawned by the still-bound outer call. */
	assert(r == 202);
	assert(w.count == 3);
	assert(w.alive[2]);

	/* The primitives still reach the world after the nested build. */
	script_eval("(define (tag ignore) (scene-name! 2 \"after\") 0)");
	assert(scene_script_call(&w, &fake_asset, "tag", 0) == 0);
	assert(strcmp(world_entity_name(&w, 2), "after") == 0);
}

/*
 * A per-entity fault inside a nested build is still caught and skipped, exactly
 * as it is for a host-driven build — and the dispatch around it survives to
 * report, so the fault is not fatal at either level.
 */
static void test_nested_build_isolates_entity_fault(void)
{
	world_reset(&w);
	script_eval("(define (load-faulty ignore)"
		    "  (let ((n (scene-build! \"(scene s"
		    "        (entity (mesh \\\"builtin://mesh/box\\\"))"
		    "        (entity (name))"
		    "        (entity (mesh \\\"builtin://mesh/torus\\\")))\")))"
		    "    (+ (* 10 n) (scene-spawn -1))))");

	/* 2 of 3 built (the middle one faulted after spawning), and the outer
	 * call still had its world when it spawned id 3. */
	assert(scene_script_call(&w, &fake_asset, "load-faulty", 0) == 23);
	assert(w.render_ref[0] == 12);
	assert(w.render_ref[2] == 11);
	assert(w.count == 4);
}

/*
 * The launcher's load path with no C in it: clear, then build. These are
 * chess_load's first two steps, driven entirely from the image.
 */
static void test_clear_then_build_reloads(void)
{
	world_reset(&w);
	assert(scene_script_build(&w, &fake_asset, SCENE_SRC) == 3);
	assert(w.count == 3);

	script_eval("(define (reload ignore)"
		    "  (scene-clear!)"
		    "  (scene-build! \"(scene next"
		    "     (entity (mesh \\\"builtin://mesh/box\\\")))\"))");
	assert(scene_script_call(&w, &fake_asset, "reload", 0) == 1);

	/* The old scene is gone, not appended to. */
	assert(w.count == 1);
	assert(w.render_ref[0] == 12);
}

int main(void)
{
	log_init();
	script_init();       /* loads the embedded scene_script.scm image */
	scene_script_init(); /* registers the scene-* host primitives */

	test_build_binds_everything();
	test_rotate_builds_quaternion();
	test_children_nest_under_parent();
	test_dispatch_and_entity_name();
	test_tick_sees_the_world();
	test_selected_tracks_the_world();
	test_world_unbound_after_tick();
	test_tick_without_hook_is_inert();
	test_destroy_named();
	test_unknown_path_is_inert();
	test_not_a_scene_form();
	test_entity_fault_is_isolated();
	test_scene_clear();
	test_scene_build_counts();
	test_scene_build_rejects();
	test_nested_build_keeps_outer_binding();
	test_nested_build_isolates_entity_fault();
	test_clear_then_build_reloads();

	printf("scene_script_test: ok\n");
	return 0;
}
