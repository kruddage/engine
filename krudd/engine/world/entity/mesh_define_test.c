/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * mesh-define! — a project bringing its own geometry, end to end against the
 * REAL asset catalog (world/asset/asset_plugin.c), not a stand-in. That is the
 * point of the test: the catalog is the thing that carries the hand-written
 * table of built-in declarations, so only the real one can prove a mesh
 * registered at runtime needs no entry in it.
 *
 * The path exercised here is the one a project source takes: evaluate a form
 * that calls (mesh-define! path src), bind the path from a (scene ...) form
 * with (scene-mesh! ...), and resolve the bytes to a mesh_blob the way the
 * renderer and the picker do (mesh_script_generate) — the only place a mesh
 * asset is ever "drawn" without a GPU.
 */
#include <entity/world.h>
#include <entity/scene_script.h>

#include "asset.h"
#include <abi/asset_api.h>
#include <asset/mesh_script.h>

#include <core/script.h>
#include <log/log.h>
#include <memory/memory.h>

#include "s7.h"

#include <assert.h>
#include <math.h>
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

static const struct memory_api test_mem_impl = {
	mem_alloc, mem_alloc_zero, mem_free,
	mem_pool_create, mem_pool_alloc, mem_pool_free, mem_pool_destroy,
};
static const struct memory_api *g_mem = &test_mem_impl;

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

/* The stable id at PATH, asserted to exist. */
static uint32_t id_at(const char *path)
{
	struct asset_info info;
	int32_t           idx = index_of(path);

	assert(idx >= 0);
	assert(asset_catalog_info((uint32_t)idx, &info) == 0);
	return info.id;
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

/* Call the mesh-define! primitive the way a project's own source would. */
static uint32_t define_mesh(const char *path, const char *src)
{
	s7_scheme *sc = script_s7();
	s7_pointer r;

	r = s7_call(sc, s7_name_to_value(sc, "mesh-define!"),
		    s7_list(sc, 2, s7_make_string(sc, path),
			    s7_make_string(sc, src)));
	assert(s7_is_integer(r));
	return (uint32_t)s7_integer(r);
}

/* Resolve a catalog entry's stored source to a mesh_blob, asserted non-NULL. */
static struct mesh_blob *blob_of(uint32_t id, const uint8_t *params,
				 uint32_t plen, uint32_t *size)
{
	const char       *src = (const char *)asset_catalog_get_data(id, NULL);
	struct mesh_blob *b;

	assert(src);
	b = mesh_script_generate(src, params, plen, g_mem, size);
	assert(b);
	return b;
}

/*
 * A project's own source, evaluated as a project's source is: plain top-level
 * forms through the shared image. It defines its geometry inline and registers
 * it — no C caller anywhere in the chain.
 */
static const char *PROJECT_SRC =
	"(define probe-mesh-src"
	"  (string-append \"(mesh probe\\n\""
	"                 \"  (generate ()\\n\""
	"                 \"    (cons (list (list -0.5 0 -0.5 0 1 0 0 0)\\n\""
	"                 \"                (list 0.5 0 -0.5 0 1 0 1 0)\\n\""
	"                 \"                (list 0.5 0 0.5 0 1 0 1 1))\\n\""
	"                 \"          (list 0 1 2))))\\n\"))"
	"(define probe-mesh-id"
	"  (mesh-define! \"project://mesh/probe\" probe-mesh-src))";

/* The (scene ...) form that binds it, exactly as chess's scene.scm does. */
static const char *SCENE_SRC =
	"(scene probe"
	"  (entity (name \"Tri\") (at 1 2 3)"
	"          (mesh \"project://mesh/probe\")))";

/*
 * The headline: a project defines a mesh from its own source, a scene binds it
 * by path, and the bytes resolve to real geometry. Nothing was seeded for this
 * path, and no declaration was written by hand for it.
 */
static void test_define_bind_and_generate(void)
{
	struct asset_info         info;
	struct mesh_blob         *b;
	const struct mesh_vertex *v;
	uint32_t                  id, e, size = 0;
	s7_scheme                *sc = script_s7();

	assert(index_of("project://mesh/probe") == -1);

	assert(script_eval(PROJECT_SRC) == 0);
	id = (uint32_t)s7_integer(s7_name_to_value(sc, "probe-mesh-id"));
	assert(id != 0);

	/* An authored, loaded ASSET_TYPE_MESH holding the source. */
	assert(asset_catalog_find(id, &info) == 0);
	assert(info.type      == ASSET_TYPE_MESH);
	assert(info.state     == ASSET_LOADED);
	assert(info.origin    == ASSET_ORIGIN_AUTHORED);
	assert(info.read_only == 0);

	/* (scene-mesh! ...) resolves the path like any built-in. */
	world_reset(&w);
	assert(scene_script_build(&w, &catalog_api, SCENE_SRC) == 1);
	e = entity_named("Tri");
	assert(w.mask[e] & COMPONENT_RENDER);
	assert(w.render_ref[e] == id);

	/* And the bound bytes are geometry: the triangle the source wrote. */
	b = blob_of(w.render_ref[e], NULL, 0, &size);
	assert(size == mesh_blob_size(3, 3));
	assert(b->magic == MESH_BLOB_MAGIC);
	assert(b->vertex_count == 3 && b->index_count == 3);
	v = mesh_blob_vertices(b);
	assert(fabsf(v[0].position[0] + 0.5f) < 1e-5f);
	assert(fabsf(v[2].position[2] - 0.5f) < 1e-5f);
	g_mem->free(b);
}

/*
 * "Renders identically to the seeded one": register a seeded built-in's own
 * source at a project path and compare the two blobs byte for byte. Same bytes
 * in, same bytes out — the resolution path a mesh asset takes does not consult
 * anything keyed by path, so a project mesh and a built-in are the same thing
 * to everything downstream of the catalog.
 */
static void test_defined_matches_seeded_byte_for_byte(void)
{
	const char       *seeded_src;
	struct mesh_blob *a, *b;
	uint32_t          sa = 0, sb = 0, id;

	seeded_src = (const char *)
		asset_catalog_get_data(id_at("builtin://mesh/cylinder"), NULL);
	assert(seeded_src);

	id = define_mesh("project://mesh/cylinder-twin", seeded_src);
	assert(id != 0);

	a = blob_of(id_at("builtin://mesh/cylinder"), NULL, 0, &sa);
	b = blob_of(id, NULL, 0, &sb);
	assert(sa == sb && sa == mesh_blob_size(150, 432));
	assert(memcmp(a, b, sa) == 0);
	g_mem->free(a);
	g_mem->free(b);
}

/*
 * The describe-vs-introspect question the issue asks to settle. It comes out
 * the same way it did for scripts in #980: no decl entry is load-bearing.
 *
 * What is proved, with the seeded heightfield's own source registered at a
 * project path:
 *   1. script_mesh_params on the bytes the catalog hands back reports exactly
 *      what it reports for the seeded heightfield — mesh-script-params parses
 *      the (params ...) clause out of the source, and there is nothing else it
 *      could be reading.
 *   2. describe() reports what the source actually says: the format constants
 *      plus the param names, derived rather than tabulated. The seeded entry's
 *      prose (surface, segments) and its vertex/index fingerprint are NOT
 *      reproduced — they are not in the source, and inventing them would mean
 *      running the generator at registration time.
 *   3. With the declaration CLEARED — describe() reporting nothing at all — the
 *      params still introspect AND still reshape the geometry. That is the
 *      proof: the declaration is inspector metadata, and the geometry does not
 *      come from it.
 */
static void test_params_need_no_decl_entry(void)
{
	struct shader_param     seeded[8], defined[8];
	struct asset_decl_field f[8];
	uint32_t                seeded_total = 0, defined_total = 0;
	uint32_t                id, seeded_id;
	int32_t                 idx;
	int                     ns, nd, i;
	const char             *src;
	struct mesh_blob       *flat;
	float                   ov[2];

	seeded_id = id_at("builtin://mesh/heightfield");
	src       = (const char *)asset_catalog_get_data(seeded_id, NULL);
	assert(src);
	ns = script_mesh_params(src, seeded, 8, &seeded_total);
	assert(ns == 2);

	id = define_mesh("project://mesh/field", src);
	assert(id != 0);

	/* 1. Same fields, read out of the bytes the catalog stores. */
	nd = script_mesh_params((const char *)asset_catalog_get_data(id, NULL),
				defined, 8, &defined_total);
	assert(nd == ns && defined_total == seeded_total);
	for (i = 0; i < nd; i++) {
		assert(strcmp(defined[i].name, seeded[i].name) == 0);
		assert(strcmp(defined[i].type, seeded[i].type) == 0);
		assert(strcmp(defined[i].edit, seeded[i].edit) == 0);
		assert(defined[i].offset     == seeded[i].offset);
		assert(defined[i].size       == seeded[i].size);
		assert(defined[i].components == seeded[i].components);
	}

	/* 2. describe() restates the source, not a table. */
	idx = index_of("project://mesh/field");
	assert(idx >= 0);
	assert(asset_catalog_describe((uint32_t)idx, f, 8) == 4);
	assert(strcmp(f[0].key, "format") == 0);
	assert(strcmp(f[0].value, "krudd-mesh") == 0);
	assert(strcmp(f[1].key, "topology") == 0);
	assert(strcmp(f[1].value, "triangles") == 0);
	assert(strcmp(f[2].key, "attributes") == 0);
	assert(strcmp(f[2].value, "position, normal, uv0") == 0);
	assert(strcmp(f[3].key, "params") == 0);
	assert(strcmp(f[3].value, "amp, freq") == 0);

	/* 3. Clear the declaration — nothing static is left anywhere. */
	assert(asset_mut_set_decl(id, NULL, 0) == 0);
	assert(asset_catalog_describe((uint32_t)idx, f, 8) == 0);

	nd = script_mesh_params((const char *)asset_catalog_get_data(id, NULL),
				defined, 8, &defined_total);
	assert(nd == 2 && defined_total == seeded_total);

	/* And the params still drive the generator: amp 0 flattens the field
	 * to y = 0 everywhere, which the un-overridden default does not. */
	ov[0] = 0.0f;   /* amp  */
	ov[1] = 1.0f;   /* freq */
	flat = blob_of(id, (const uint8_t *)ov, sizeof(ov), NULL);
	{
		const struct mesh_vertex *v = mesh_blob_vertices(flat);
		uint32_t                  k;
		int                       bumpy = 0;

		for (k = 0; k < flat->vertex_count; k++)
			assert(fabsf(v[k].position[1]) < 1e-6f);
		g_mem->free(flat);

		flat = blob_of(id, NULL, 0, NULL);
		v = mesh_blob_vertices(flat);
		for (k = 0; k < flat->vertex_count; k++)
			if (fabsf(v[k].position[1]) > 1e-6f)
				bumpy = 1;
		assert(bumpy);
		g_mem->free(flat);
	}
}

/*
 * Redefinition REPLACES: the second call keeps the stable id, so an entity
 * bound before the redefinition draws the new geometry with nothing to rebind.
 * This is what makes re-evaluating a project source a reload rather than an
 * error — and for a mesh it is also what keeps a live scene from losing its
 * geometry to a stale id halfway through a reload.
 */
static void test_redefine_replaces_in_place(void)
{
	struct mesh_blob *b;
	uint32_t          first, again, e;

	first = define_mesh("project://mesh/iter",
			    "(mesh iter (generate ()"
			    " (cons (list (list 0 0 0  0 1 0  0 0)"
			    "             (list 1 0 0  0 1 0  1 0)"
			    "             (list 0 0 1  0 1 0  0 1))"
			    "       (list 0 1 2))))");
	assert(first != 0);

	world_reset(&w);
	assert(scene_script_build(&w, &catalog_api,
		"(scene i (entity (name \"pin\")"
		"          (mesh \"project://mesh/iter\")))") == 1);
	e = entity_named("pin");
	assert(w.render_ref[e] == first);

	b = blob_of(w.render_ref[e], NULL, 0, NULL);
	assert(b->vertex_count == 3);
	g_mem->free(b);

	/* Redefine the same path with more geometry and a params clause. */
	again = define_mesh("project://mesh/iter",
			    "(mesh iter (params (lift float (default 2)))"
			    " (generate ()"
			    " (cons (list (list 0 (param 'lift) 0  0 1 0  0 0)"
			    "             (list 1 0 0  0 1 0  1 0)"
			    "             (list 0 0 1  0 1 0  0 1)"
			    "             (list 1 0 1  0 1 0  1 1))"
			    "       (list 0 1 2  1 3 2))))");
	assert(again == first);          /* the identity survived the rewrite */

	/* The bound entity was never touched, yet it draws the new source. */
	assert(w.render_ref[e] == first);
	b = blob_of(w.render_ref[e], NULL, 0, NULL);
	assert(b->vertex_count == 4 && b->index_count == 6);
	assert(fabsf(mesh_blob_vertices(b)[0].position[1] - 2.0f) < 1e-5f);
	g_mem->free(b);

	/* The declaration was republished from the new source. */
	{
		struct asset_decl_field f[8];
		int32_t                 idx = index_of("project://mesh/iter");

		assert(idx >= 0);
		assert(asset_catalog_describe((uint32_t)idx, f, 8) == 4);
		assert(strcmp(f[3].key, "params") == 0);
		assert(strcmp(f[3].value, "lift") == 0);
	}
}

/*
 * A path the engine seeded read-only is refused, and left alone: a project
 * shadowing a built-in mesh would change every scene binding it, not just its
 * own. A path already holding another type is refused too — a mesh path quietly
 * holding script bytes would draw nothing and explain nothing. Bad arguments
 * are refused the same way — 0, never a fault.
 */
static void test_builtin_and_bad_args_refused(void)
{
	s7_scheme  *sc = script_s7();
	s7_pointer  fn = s7_name_to_value(sc, "mesh-define!");
	const char *before;
	s7_pointer  r;
	uint32_t    box;

	box    = id_at("builtin://mesh/box");
	before = (const char *)asset_catalog_get_data(box, NULL);
	assert(before);
	assert(define_mesh("builtin://mesh/box", "(mesh hijack)") == 0);
	assert(strcmp((const char *)asset_catalog_get_data(box, NULL),
		      before) == 0);

	/* A script path is not a mesh path, even an authored one. */
	r = s7_call(sc, s7_name_to_value(sc, "script-define!"),
		    s7_list(sc, 2, s7_make_string(sc, "project://script/s"),
			    s7_make_string(sc, "(script s)")));
	assert(s7_is_integer(r) && s7_integer(r) != 0);
	assert(define_mesh("project://script/s", "(mesh nope)") == 0);
	assert(strncmp((const char *)
		       asset_catalog_get_data((uint32_t)s7_integer(r), NULL),
		       "(script s)", 10) == 0);

	/* Non-string arguments degrade to 0 rather than trapping. */
	r = s7_call(sc, fn, s7_list(sc, 2, s7_make_integer(sc, 1),
				    s7_make_string(sc, "(mesh x)")));
	assert(s7_is_integer(r) && s7_integer(r) == 0);
	r = s7_call(sc, fn, s7_list(sc, 2,
				    s7_make_string(sc, "project://mesh/nah"),
				    s7_make_integer(sc, 2)));
	assert(s7_is_integer(r) && s7_integer(r) == 0);
	assert(index_of("project://mesh/nah") == -1);
}

/*
 * With no catalog bound — a host that runs the image but has no asset
 * subsystem, which is what the headless rules tests are — the primitive is
 * inert: it reports 0 and nothing else happens.
 */
static void test_inert_without_catalog(void)
{
	scene_script_bind_catalog(NULL, NULL);
	assert(define_mesh("project://mesh/void", "(mesh void)") == 0);
	assert(index_of("project://mesh/void") == -1);
	scene_script_bind_catalog(&catalog_api, &mut_api);
}

int main(void)
{
	mem_init();
	log_init();
	asset_init();        /* the real catalog, seeded built-ins and all */
	script_init();       /* loads mesh_script.scm + scene_script.scm   */
	scene_script_bind_catalog(&catalog_api, &mut_api);
	scene_script_init(); /* the scene-* primitives and mesh-define!    */

	test_define_bind_and_generate();
	test_defined_matches_seeded_byte_for_byte();
	test_params_need_no_decl_entry();
	test_redefine_replaces_in_place();
	test_builtin_and_bad_args_refused();
	test_inert_without_catalog();

	printf("mesh_define_test: ok\n");
	return 0;
}
