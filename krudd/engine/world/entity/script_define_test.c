/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * script-define! — a project bringing its own entity scripts, end to end
 * against the REAL asset catalog (world/asset/asset_plugin.c), not a stand-in.
 * That is the point of the test: the catalog is the thing that carries the
 * hand-written table of built-in declarations, so only the real one can prove a
 * script registered at runtime needs no entry in it.
 *
 * The path exercised here is the one a project source takes: evaluate a form
 * that calls (script-define! path src), bind the path from a (scene ...) form
 * with (scene-script! ...), and drive a frame the way the scene tick does.
 */
#include <entity/world.h>
#include <entity/scene_script.h>
#include "entity_script.h"

#include "asset.h"
#include <abi/asset_api.h>

#include <core/script.h>
#include <log/log.h>
#include <memory/memory.h>

#include "s7.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* One world instance reused across the checks; too big for the stack. */
static struct world w;

/*
 * The real catalog's two vtables, assembled here from asset.h the way the asset
 * plugin assembles them for the subsystem manager. Going direct keeps the test
 * free of the subsystem manager while still exercising the real storage, the
 * real read-only built-ins, and the real static declaration table.
 */
static const struct asset_api catalog_api = {
	.count    = asset_catalog_count,
	.info     = asset_catalog_info,
	.describe = asset_catalog_describe,
	.find     = asset_catalog_find,
	.get_data = asset_catalog_get_data,
};

static const struct asset_mut_api mut_api = {
	.create   = asset_mut_create,
	.set_data = asset_mut_set_data,
	.destroy  = asset_mut_destroy,
	.set_decl = asset_mut_set_decl,
	.inject   = asset_mut_inject,
};

static int feq(float a, float b)
{
	float d = a - b;

	return (d < 0.0f ? -d : d) < 1e-4f;
}

/*
 * Evaluate SRC as a project's own source is evaluated — plain top-level forms
 * through the shared image — then read back the integer left in NAME. This is
 * how a project reaches script-define!: no C caller, just its own Scheme.
 */
static uint32_t eval_id(const char *src, const char *name)
{
	s7_scheme *sc = script_s7();
	s7_pointer v;

	assert(script_eval(src) == 0);
	v = s7_name_to_value(sc, name);
	return s7_is_integer(v) ? (uint32_t)s7_integer(v) : 0;
}

/* The catalog index of PATH, or -1 when it holds nothing. */
static int32_t index_of(const char *path)
{
	struct asset_info info;
	uint32_t          i, n = asset_catalog_count();

	for (i = 0; i < n; i++) {
		if (asset_catalog_info(i, &info) == 0
		    && strcmp(info.path, path) == 0)
			return (int32_t)i;
	}
	return -1;
}

/* The entity index carrying NAME, asserted to exist. */
static uint32_t entity_named(const char *name)
{
	uint32_t e;

	for (e = 0; e < w.count; e++) {
		const char *n = world_entity_name(&w, e);

		if (n && strcmp(n, name) == 0)
			return e;
	}
	assert(0 && "no such entity");
	return 0;
}

/*
 * A camera script of chess's exact shape — a project's own script, defined from
 * its own source. The dispatch target is defined here the way rules.scm defines
 * chess-camera-tick!: the script asset is a one-line wrapper, the behaviour is
 * an ordinary procedure in the image.
 */
static const char *PROJECT_SRC =
	"(define (probe-cam-tick! self t)"
	"  (entity-set-position! self 1.5 2.5 3.5))"
	"(define probe-cam-src"
	"  (string-append \"(script probe-cam\\n\""
	"                 \"  (on-tick (self t)\\n\""
	"                 \"    (probe-cam-tick! self t)))\\n\"))"
	"(define probe-cam-id"
	"  (script-define! \"project://script/probe-cam\" probe-cam-src))";

/* The (scene ...) form that binds it, exactly as chess's scene.scm does. */
static const char *SCENE_SRC =
	"(scene probe"
	"  (entity (name \"Camera\") (at 5.5 8.5 10.5)"
	"          (script \"project://script/probe-cam\")))";

/*
 * The headline: a project defines a script from its own source, a scene binds
 * it by path, and the entity ticks it. Nothing was seeded for this path.
 */
static void test_define_bind_and_tick(void)
{
	uint32_t          id, e;
	struct asset_info info;

	assert(index_of("project://script/probe-cam") == -1);

	id = eval_id(PROJECT_SRC, "probe-cam-id");
	assert(id != 0);

	/* An authored, loaded ASSET_TYPE_SCRIPT holding the source. */
	assert(asset_catalog_find(id, &info) == 0);
	assert(info.type      == ASSET_TYPE_SCRIPT);
	assert(info.state     == ASSET_LOADED);
	assert(info.origin    == ASSET_ORIGIN_AUTHORED);
	assert(info.read_only == 0);
	assert(strncmp((const char *)asset_catalog_get_data(id, NULL),
		       "(script probe-cam", 17) == 0);

	/* (scene-script! ...) resolves the path like any built-in. */
	world_reset(&w);
	assert(scene_script_build(&w, &catalog_api, SCENE_SRC) == 1);
	e = entity_named("Camera");
	assert(w.mask[e] & COMPONENT_SCRIPT);
	assert(w.script_ref[e] == id);

	/* One frame: propagate the rest pose, then run the scripts over it. */
	world_tick(&w, 16.0f);
	entity_script_tick(&w, &catalog_api, 1.0f);
	assert(feq(w.world_xform[e].position[0], 1.5f));
	assert(feq(w.world_xform[e].position[1], 2.5f));
	assert(feq(w.world_xform[e].position[2], 3.5f));

	/* The authored rest pose is untouched, as for a seeded script. */
	assert(feq(w.local[e].position[0], 5.5f));
}

/*
 * The params question the issue asks to verify. A script defined at runtime
 * carries the same (params ...) clause a seeded built-in does; here that clause
 * is pulse's, byte for byte, registered at a project path.
 *
 * What is proved:
 *   1. script_entity_params on the bytes the catalog hands back reports exactly
 *      what it reports for the seeded pulse — it parses the source, and
 *      there is nothing else it could be reading.
 *   2. describe() reports the same three fields the seeded pulse's hand-written
 *      table entry carries, derived from the source instead.
 *   3. With the declaration CLEARED — describe() reporting nothing at all — the
 *      params still introspect and the script still ticks its params. That is
 *      the proof that no static decl entry is load-bearing: the declaration is
 *      inspector metadata, and the parameters do not come from it.
 */
static void test_params_need_no_decl_entry(void)
{
	struct shader_param  seeded[8], defined[8];
	struct asset_decl_field f[8];
	uint32_t             seeded_total = 0, defined_total = 0;
	uint32_t             id, e;
	int32_t              seeded_idx, defined_idx;
	int                  ns, nd, i;
	float                ov[2];

	seeded_idx = index_of("builtin://script/pulse");
	assert(seeded_idx >= 0);

	/* Register the seeded pulse's own source under a project path. */
	{
		struct asset_info info;
		const char       *pulse_src;
		s7_scheme        *sc = script_s7();
		s7_pointer        fn, r;

		assert(asset_catalog_info((uint32_t)seeded_idx, &info) == 0);
		pulse_src = (const char *)asset_catalog_get_data(info.id, NULL);
		assert(pulse_src);

		fn = s7_name_to_value(sc, "script-define!");
		r  = s7_call(sc, fn,
			     s7_list(sc, 2,
				     s7_make_string(sc,
						    "project://script/twin"),
				     s7_make_string(sc, pulse_src)));
		assert(s7_is_integer(r));
		id = (uint32_t)s7_integer(r);
		assert(id != 0);

		ns = script_entity_params(pulse_src, seeded, 8, &seeded_total);
	}

	/* 1. Same fields, read out of the bytes the catalog stores. */
	nd = script_entity_params(
		(const char *)asset_catalog_get_data(id, NULL), defined, 8,
		&defined_total);
	assert(nd == ns && nd == 2);
	assert(defined_total == seeded_total);
	for (i = 0; i < nd; i++) {
		assert(strcmp(defined[i].name, seeded[i].name) == 0);
		assert(strcmp(defined[i].type, seeded[i].type) == 0);
		assert(strcmp(defined[i].edit, seeded[i].edit) == 0);
		assert(defined[i].offset == seeded[i].offset);
		assert(defined[i].size == seeded[i].size);
		assert(defined[i].components == seeded[i].components);
		assert(feq(defined[i].edit_min, seeded[i].edit_min));
		assert(feq(defined[i].edit_max, seeded[i].edit_max));
	}

	/* 2. describe() matches the seeded entry's table fields. */
	defined_idx = index_of("project://script/twin");
	assert(defined_idx >= 0);
	assert(asset_catalog_describe((uint32_t)defined_idx, f, 8) == 3);
	assert(strcmp(f[0].key, "format") == 0);
	assert(strcmp(f[0].value, "krudd-script") == 0);
	assert(strcmp(f[1].key, "hooks") == 0);
	assert(strcmp(f[1].value, "on-tick") == 0);
	assert(strcmp(f[2].key, "params") == 0);
	assert(strcmp(f[2].value, "amp, rate") == 0);
	{
		struct asset_decl_field g[8];
		uint32_t                n;

		n = asset_catalog_describe((uint32_t)seeded_idx, g, 8);
		assert(n == 3);
		for (i = 0; i < 3; i++) {
			assert(strcmp(f[i].key, g[i].key) == 0);
			assert(strcmp(f[i].value, g[i].value) == 0);
		}
	}

	/* 3. Clear the declaration — nothing static is left anywhere. */
	assert(asset_mut_set_decl(id, NULL, 0) == 0);
	assert(asset_catalog_describe((uint32_t)defined_idx, f, 8) == 0);

	nd = script_entity_params(
		(const char *)asset_catalog_get_data(id, NULL), defined, 8,
		&defined_total);
	assert(nd == 2 && defined_total == seeded_total);

	/* And the params still drive the script: pulse scales by amp/rate,
	 * and an override of (amp 1, rate 0) parks the scale at 1+sin(0). */
	world_reset(&w);
	assert(scene_script_build(&w, &catalog_api,
		"(scene p (entity (name \"pulser\")"
		"          (script \"project://script/twin\")))") == 1);
	e = entity_named("pulser");
	ov[0] = 1.0f;   /* amp  */
	ov[1] = 0.0f;   /* rate */
	world_set_script_params(&w, e, (const uint8_t *)ov, sizeof(ov));
	world_tick(&w, 16.0f);
	entity_script_tick(&w, &catalog_api, 2.0f);
	assert(feq(w.world_xform[e].scale[0], 1.0f));

	/* A non-zero rate with the same amp moves it off 1 — the params are
	 * read, not defaulted away, with no declaration in the catalog. */
	ov[1] = 1.0f;
	world_set_script_params(&w, e, (const uint8_t *)ov, sizeof(ov));
	world_tick(&w, 16.0f);
	entity_script_tick(&w, &catalog_api, 2.0f);
	assert(!feq(w.world_xform[e].scale[0], 1.0f));
}

/*
 * Redefinition REPLACES: the second call keeps the stable id, so an entity
 * bound before the redefinition runs the new source with nothing to rebind.
 * This is what makes re-evaluating a project source a reload rather than an
 * error.
 */
static void test_redefine_replaces_in_place(void)
{
	s7_scheme *sc = script_s7();
	s7_pointer fn = s7_name_to_value(sc, "script-define!");
	uint32_t   first, again, e;
	s7_pointer r;

	r = s7_call(sc, fn,
		    s7_list(sc, 2, s7_make_string(sc, "project://script/iter"),
			    s7_make_string(sc,
				"(script iter (on-tick (self t)"
				" (entity-set-position! self 7 0 0)))")));
	first = (uint32_t)s7_integer(r);
	assert(first != 0);

	world_reset(&w);
	assert(scene_script_build(&w, &catalog_api,
		"(scene i (entity (name \"pin\")"
		"          (script \"project://script/iter\")))") == 1);
	e = entity_named("pin");
	world_tick(&w, 16.0f);
	entity_script_tick(&w, &catalog_api, 1.0f);
	assert(feq(w.world_xform[e].position[0], 7.0f));

	/* Redefine the same path with a different body and a params clause. */
	r = s7_call(sc, fn,
		    s7_list(sc, 2, s7_make_string(sc, "project://script/iter"),
			    s7_make_string(sc,
				"(script iter"
				" (params (shift float (default 9)))"
				" (on-tick (self t)"
				"  (entity-set-position!"
				"   self (param 'shift) 0 0)))")));
	again = (uint32_t)s7_integer(r);
	assert(again == first);          /* the identity survived the rewrite */

	/* The bound entity was never touched, yet it runs the new source. */
	assert(w.script_ref[e] == first);
	world_tick(&w, 16.0f);
	entity_script_tick(&w, &catalog_api, 1.0f);
	assert(feq(w.world_xform[e].position[0], 9.0f));

	/* The declaration was republished from the new source. */
	{
		struct asset_decl_field f[8];
		int32_t                 idx = index_of("project://script/iter");

		assert(idx >= 0);
		assert(asset_catalog_describe((uint32_t)idx, f, 8) == 3);
		assert(strcmp(f[2].value, "shift") == 0);
	}
}

/*
 * A path the engine seeded read-only is refused, and left alone: a project
 * shadowing a built-in would change every scene binding it, not just its own.
 * Bad arguments are refused the same way — 0, never a fault.
 */
static void test_builtin_and_bad_args_refused(void)
{
	s7_scheme  *sc = script_s7();
	s7_pointer  fn = s7_name_to_value(sc, "script-define!");
	const char *before;
	s7_pointer  r;
	int32_t     idx;
	struct asset_info info;

	idx = index_of("builtin://script/spinner");
	assert(idx >= 0);
	assert(asset_catalog_info((uint32_t)idx, &info) == 0);
	before = (const char *)asset_catalog_get_data(info.id, NULL);
	assert(before);

	r = s7_call(sc, fn,
		    s7_list(sc, 2,
			    s7_make_string(sc, "builtin://script/spinner"),
			    s7_make_string(sc, "(script hijack)")));
	assert(s7_is_integer(r) && s7_integer(r) == 0);
	assert(strcmp((const char *)asset_catalog_get_data(info.id, NULL),
		      before) == 0);

	/* Non-string arguments degrade to 0 rather than trapping. */
	r = s7_call(sc, fn, s7_list(sc, 2, s7_make_integer(sc, 1),
				    s7_make_string(sc, "(script x)")));
	assert(s7_is_integer(r) && s7_integer(r) == 0);
	r = s7_call(sc, fn, s7_list(sc, 2,
				    s7_make_string(sc, "project://script/nah"),
				    s7_make_integer(sc, 2)));
	assert(s7_is_integer(r) && s7_integer(r) == 0);
	assert(index_of("project://script/nah") == -1);
}

/*
 * With no catalog bound — a host that runs the image but has no asset
 * subsystem, which is what the headless rules tests are — the primitive is
 * inert: it reports 0 and nothing else happens.
 */
static void test_inert_without_catalog(void)
{
	s7_scheme *sc = script_s7();
	s7_pointer fn = s7_name_to_value(sc, "script-define!");
	s7_pointer r;

	scene_script_bind_catalog(NULL, NULL);
	r = s7_call(sc, fn,
		    s7_list(sc, 2, s7_make_string(sc, "project://script/void"),
			    s7_make_string(sc, "(script void)")));
	assert(s7_is_integer(r) && s7_integer(r) == 0);
	assert(index_of("project://script/void") == -1);
	scene_script_bind_catalog(&catalog_api, &mut_api);
}

int main(void)
{
	mem_init();
	log_init();
	asset_init();         /* the real catalog, seeded built-ins and all */
	script_init();        /* loads entity_script.scm + scene_script.scm */
	entity_script_init(); /* the entity-* primitives a clause calls      */
	scene_script_bind_catalog(&catalog_api, &mut_api);
	scene_script_init();  /* the scene-* primitives and script-define!   */

	test_define_bind_and_tick();
	test_params_need_no_decl_entry();
	test_redefine_replaces_in_place();
	test_builtin_and_bad_args_refused();
	test_inert_without_catalog();

	printf("script_define_test: ok\n");
	return 0;
}
