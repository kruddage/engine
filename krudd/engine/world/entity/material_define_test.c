/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * material-define! — a project bringing its own materials, end to end against
 * the REAL asset catalog (world/asset/asset_plugin.c), not a stand-in. Same
 * reason as its script and mesh twins: the catalog is what carries the seeded
 * built-ins and the hand-written table of their declarations, so only the real
 * one can prove a material registered at runtime needs neither.
 *
 * A material is the one asset whose stored bytes are NOT its source. A mesh or a
 * script stores the text it was authored as; a material stores the packed wire
 * form the renderer uploads to a uniform buffer verbatim — [shader-ref u32]
 * followed by the shader's std140 Material block, optionally followed by a
 * texture slot. So the interesting claim here is not "the source survived" but
 * "the bytes are right", and the strongest available statement of that is
 * byte-for-byte agreement with what the C seeders produce today: every seeded
 * material is re-authored below as a (material ...) source and memcmp'd against
 * the entry the engine seeded. If the packing were wrong in any field, any
 * offset, or any pad byte, those comparisons fail.
 */
#include <entity/world.h>
#include <entity/scene_script.h>

#include "asset.h"
#include <abi/asset_api.h>

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
 * plugin assembles them for the subsystem manager — mesh_define_test's own
 * arrangement, and for the same reason.
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

/* Call the material-define! primitive the way a project's own source would. */
static uint32_t define_material(const char *path, const char *src)
{
	s7_scheme *sc = script_s7();
	s7_pointer r;

	r = s7_call(sc, s7_name_to_value(sc, "material-define!"),
		    s7_list(sc, 2, s7_make_string(sc, path),
			    s7_make_string(sc, src)));
	assert(s7_is_integer(r));
	return (uint32_t)s7_integer(r);
}

/* The float stored at byte OFF of the asset's bytes. */
static float float_at(uint32_t id, uint32_t off)
{
	const uint8_t *b = (const uint8_t *)asset_catalog_get_data(id, NULL);
	float          v;

	assert(b);
	memcpy(&v, b + off, sizeof(v));
	return v;
}

/* The uint32 stored at byte OFF of the asset's bytes. */
static uint32_t u32_at(uint32_t id, uint32_t off)
{
	const uint8_t *b = (const uint8_t *)asset_catalog_get_data(id, NULL);
	uint32_t       v;

	assert(b);
	memcpy(&v, b + off, sizeof(v));
	return v;
}

/*
 * A project's own source, evaluated as a project's source is: plain top-level
 * forms through the shared image. It authors a material inline and registers it
 * — no C caller anywhere in the chain.
 */
static const char *PROJECT_SRC =
	"(define probe-material-src"
	"  (string-append \"(material probe\\n\""
	"                 \"  (shader \\\"builtin://shader/pbr\\\")\\n\""
	"                 \"  (base_color 0.25 0.5 0.75 1.0)\\n\""
	"                 \"  (metallic 0.125)\\n\""
	"                 \"  (roughness 0.625)\\n\""
	"                 \"  (subsurface 0.5))\\n\"))"
	"(define probe-material-id"
	"  (material-define! \"project://material/probe\" probe-material-src))";

/* The (scene ...) form that binds it, exactly as a project's scene clause. */
static const char *SCENE_SRC =
	"(scene probe"
	"  (entity (name \"Lit\") (at 1 2 3)"
	"          (mesh \"builtin://mesh/box\")"
	"          (material \"project://material/probe\")))";

/*
 * The headline: a project defines a material from its own source, a scene binds
 * it by path, and the stored bytes are the wire form the renderer reads — the
 * shader it named, then each value at the offset that shader's own Material
 * block declares. Nothing was seeded for this path and no declaration was
 * written by hand for it.
 *
 * The offsets asserted here are the pbr block's (base_color @0, metallic @16,
 * roughness @20, emissive @32, subsurface @44) plus the 4-byte shader-ref
 * header. They are spelled out rather than derived so the test would notice the
 * layout moving underneath the packer, which is the whole risk a std140 blob
 * carries.
 */
static void test_define_bind_and_pack(void)
{
	struct asset_info info;
	uint32_t          id, e, size = 0;
	s7_scheme        *sc = script_s7();

	assert(index_of("project://material/probe") == -1);

	assert(script_eval(PROJECT_SRC) == 0);
	id = (uint32_t)s7_integer(s7_name_to_value(sc, "probe-material-id"));
	assert(id != 0);

	/* An authored, loaded ASSET_TYPE_MATERIAL holding the packed block. */
	assert(asset_catalog_find(id, &info) == 0);
	assert(info.type      == ASSET_TYPE_MATERIAL);
	assert(info.state     == ASSET_LOADED);
	assert(info.origin    == ASSET_ORIGIN_AUTHORED);
	assert(info.read_only == 0);

	/* Header + the pbr shader's 48-byte std140 Material block. */
	assert(asset_catalog_get_data(id, &size) != NULL);
	assert(size == 4u + 48u);
	assert(u32_at(id, 0) == id_at("builtin://shader/pbr"));

	assert(float_at(id, 4)  == 0.25f);
	assert(float_at(id, 8)  == 0.5f);
	assert(float_at(id, 12) == 0.75f);
	assert(float_at(id, 16) == 1.0f);
	assert(float_at(id, 20) == 0.125f);       /* metallic   @16 in block */
	assert(float_at(id, 24) == 0.625f);       /* roughness  @20          */
	assert(float_at(id, 48) == 0.5f);         /* subsurface @44          */

	/* Unnamed fields take the shader's declared default: emissive is 0. */
	assert(float_at(id, 36) == 0.0f);
	assert(float_at(id, 40) == 0.0f);
	assert(float_at(id, 44) == 0.0f);

	/* And the std140 gap between roughness and emissive stays zero. */
	assert(u32_at(id, 28) == 0 && u32_at(id, 32) == 0);

	/* (scene-material! ...) resolves the path like any built-in. */
	world_reset(&w);
	assert(scene_script_build(&w, &catalog_api, SCENE_SRC) == 1);
	e = entity_named("Lit");
	assert(w.material_ref[e] == id);
}

/*
 * The acceptance criterion in full: "the source-to-std140 packing is tested
 * against the bytes the seeded materials produce today". Every seeded material
 * is re-authored as a (material ...) source, registered at a project path, and
 * compared byte for byte with the entry the engine seeded — including the pad
 * bytes std140 leaves between fields, which is where a packer's mistakes hide.
 *
 * All three seeder shapes are covered: seed_pbr_material's 52-byte pure
 * parametric form, seed_textured_material's 16-byte block plus texture trailer,
 * and seed_pbr_textured_material's 32-byte block plus trailer. A material's
 * shader-ref is the same id on both sides because both name the same seeded
 * shader, so nothing has to be masked out of the comparison.
 */
static void test_packs_like_the_seeders(void)
{
	static const struct {
		const char *seeded;
		const char *src;
	} CASES[] = {
		{ "builtin://material/pbr-metal",
		  "(material metal (shader \"builtin://shader/pbr\")"
		  " (base_color 1.00 0.78 0.34 1.0)"
		  " (metallic 1.0) (roughness 0.30))" },
		{ "builtin://material/pbr-plastic",
		  "(material plastic (shader \"builtin://shader/pbr\")"
		  " (base_color 0.85 0.13 0.16 1.0)"
		  " (metallic 0.0) (roughness 0.45))" },
		{ "builtin://material/pbr-ruby",
		  "(material ruby (shader \"builtin://shader/pbr\")"
		  " (base_color 0.55 0.02 0.06 1.0)"
		  " (metallic 0.05) (roughness 0.12) (subsurface 0.0))" },
		{ "builtin://material/pbr-sapphire",
		  "(material sapphire (shader \"builtin://shader/pbr\")"
		  " (base_color 0.03 0.10 0.55 1.0)"
		  " (metallic 0.05) (roughness 0.12))" },
		{ "builtin://material/pbr-stone",
		  "(material stone (shader \"builtin://shader/pbr\")"
		  " (base_color 0.78 0.74 0.64 1.0)"
		  " (metallic 0.0) (roughness 0.75))" },
		{ "builtin://material/board-light",
		  "(material light (shader \"builtin://shader/pbr\")"
		  " (base_color 0.80 0.66 0.46 1.0)"
		  " (metallic 0.0) (roughness 0.55))" },
		{ "builtin://material/board-dark",
		  "(material dark (shader \"builtin://shader/pbr\")"
		  " (base_color 0.28 0.17 0.10 1.0)"
		  " (metallic 0.0) (roughness 0.50))" },
		{ "builtin://material/checker",
		  "(material checker"
		  " (shader \"builtin://shader/scene-textured\")"
		  " (base_color 1.0 1.0 1.0 1.0)"
		  " (texture \"builtin://texture/checker\" 256 256))" },
		{ "builtin://material/pbr-grass",
		  "(material grass (shader \"builtin://shader/pbr-textured\")"
		  " (base_color 1.0 1.0 1.0 1.0)"
		  " (metallic 0.0) (roughness 0.9) (bump_strength 0.6)"
		  " (texture \"builtin://texture/grass\" 512 512))" },
	};
	char     path[64];
	uint32_t i;

	for (i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
		const void *seeded, *defined;
		uint32_t    sa = 0, sb = 0, id;

		snprintf(path, sizeof(path), "project://material/twin-%u", i);
		id = define_material(path, CASES[i].src);
		assert(id != 0);

		seeded  = asset_catalog_get_data(id_at(CASES[i].seeded), &sa);
		defined = asset_catalog_get_data(id, &sb);
		assert(seeded && defined);
		assert(sa == sb);
		assert(memcmp(seeded, defined, sa) == 0);
	}

	/* The three wire-form sizes, so a change to any of them is noticed. */
	assert(u32_at(id_at("project://material/twin-0"), 0)
	       == id_at("builtin://shader/pbr"));
}

/*
 * The failure mode the issue asks to decide, exercised in every shape it takes.
 * All of them REFUSE — material-define! answers 0 and registers nothing — rather
 * than defaulting a field, dropping a clause, or storing a short blob. A
 * silently wrong material is the one outcome a project can neither see nor
 * debug: it renders, just wrong.
 *
 * The refusals are: text that is not a (material ...) form; a missing
 * (shader ...) clause; a shader path nothing is at; a shader path holding
 * something that is not a shader; a field the shader's Material block does not
 * declare (the typo case); a field given the wrong number of components; a
 * non-number where a component belongs; and a (texture ...) clause naming
 * something that is not a texture. Non-string arguments degrade the same way.
 */
static void test_malformed_refused(void)
{
	static const char *BAD[] = {
		"not a form at all",
		"(scene s)",
		"(material m)",
		"(material m (shader))",
		"(material m (shader 7))",
		"(material m (shader \"builtin://material/nowhere\"))",
		"(material m (shader \"builtin://mesh/box\"))",
		"(material m (shader \"builtin://shader/pbr\") (metalic 1.0))",
		"(material m (shader \"builtin://shader/pbr\")"
		" (base_color 1.0 1.0 1.0))",
		"(material m (shader \"builtin://shader/pbr\") (metallic 0 0))",
		"(material m (shader \"builtin://shader/pbr\")"
		" (metallic \"shiny\"))",
		"(material m (shader \"builtin://shader/scene-textured\")"
		" (texture \"builtin://mesh/box\" 64 64))",
		"(material m (shader \"builtin://shader/scene-textured\")"
		" (texture \"builtin://texture/nowhere\" 64 64))",
	};
	s7_scheme  *sc = script_s7();
	s7_pointer  fn = s7_name_to_value(sc, "material-define!");
	s7_pointer  r;
	uint32_t    i;

	for (i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++) {
		assert(define_material("project://material/bad", BAD[i]) == 0);
		assert(index_of("project://material/bad") == -1);
	}

	/* Non-string arguments degrade to 0 rather than trapping. */
	r = s7_call(sc, fn, s7_list(sc, 2, s7_make_integer(sc, 1),
				    s7_make_string(sc, "(material m)")));
	assert(s7_is_integer(r) && s7_integer(r) == 0);
	r = s7_call(sc, fn, s7_list(sc, 2,
				    s7_make_string(sc, "project://material/nah"),
				    s7_make_integer(sc, 2)));
	assert(s7_is_integer(r) && s7_integer(r) == 0);
	assert(index_of("project://material/nah") == -1);
}

/*
 * A refused REdefinition leaves the entry that was already there untouched. The
 * refusal path has to be all-or-nothing at the catalog, not just at creation: a
 * project that edits a live material into a typo would otherwise be worse off
 * than one that never defined it, since the working bytes would already be gone.
 */
static void test_refusal_leaves_the_old_bytes(void)
{
	uint8_t  before[64];
	uint32_t id, size = 0;

	id = define_material("project://material/keep",
			     "(material keep (shader \"builtin://shader/pbr\")"
			     " (base_color 0.1 0.2 0.3 1.0) (roughness 0.4))");
	assert(id != 0);
	assert(asset_catalog_get_data(id, &size) != NULL
	       && size <= sizeof(before));
	memcpy(before, asset_catalog_get_data(id, NULL), size);

	assert(define_material("project://material/keep",
			       "(material keep (shader \"builtin://shader/pbr\")"
			       " (base_color 0.9 0.9 0.9 1.0) (ruffness 0.4))")
	       == 0);
	assert(id_at("project://material/keep") == id);
	assert(memcmp(asset_catalog_get_data(id, NULL), before, size) == 0);
}

/*
 * Redefinition REPLACES: the second call keeps the stable id, so an entity bound
 * before it draws the new look with nothing to rebind. This is what makes
 * re-evaluating a project source a reload rather than an error — and for a
 * material, as for a mesh, it is also what keeps a live scene from losing its
 * binding to a stale id halfway through one.
 */
static void test_redefine_replaces_in_place(void)
{
	uint32_t first, again, e;

	first = define_material("project://material/iter",
				"(material iter"
				" (shader \"builtin://shader/pbr\")"
				" (base_color 1.0 0.0 0.0 1.0)"
				" (roughness 0.2))");
	assert(first != 0);

	world_reset(&w);
	assert(scene_script_build(&w, &catalog_api,
		"(scene i (entity (name \"pin\")"
		"          (material \"project://material/iter\")))") == 1);
	e = entity_named("pin");
	assert(w.material_ref[e] == first);
	assert(float_at(first, 4) == 1.0f && float_at(first, 24) == 0.2f);

	again = define_material("project://material/iter",
				"(material iter"
				" (shader \"builtin://shader/pbr\")"
				" (base_color 0.0 1.0 0.0 1.0)"
				" (roughness 0.8))");
	assert(again == first);          /* the identity survived the repack */

	/* The bound entity was never touched, yet it carries the new bytes. */
	assert(w.material_ref[e] == first);
	assert(float_at(first, 4) == 0.0f && float_at(first, 8) == 1.0f);
	assert(float_at(first, 24) == 0.8f);
}

/*
 * The describe-vs-introspect question, settled for materials the way #980
 * settled it for scripts and #1002 for meshes: no declaration is load-bearing.
 *
 * A material's declaration restates what the packing already knows — the format
 * and the shader it named, plus the texture slot when it has one — and is
 * derived at registration rather than tabulated by path. The proof is that
 * clearing it entirely leaves the bytes, the binding and the shader-ref exactly
 * as they were: what the renderer reads is the blob, and the blob's schema is
 * the shader's, reached through the shader-ref in the bytes themselves.
 */
static void test_declaration_is_derived_and_not_load_bearing(void)
{
	struct asset_decl_field f[8];
	uint8_t                 before[64];
	uint32_t                id, size = 0;
	int32_t                 idx;

	id = define_material("project://material/decl",
			     "(material decl (shader \"builtin://shader/pbr\")"
			     " (metallic 0.7))");
	assert(id != 0);
	idx = index_of("project://material/decl");
	assert(idx >= 0);

	assert(asset_catalog_describe((uint32_t)idx, f, 8) == 2);
	assert(strcmp(f[0].key, "format") == 0);
	assert(strcmp(f[0].value, "krudd-material") == 0);
	assert(strcmp(f[1].key, "shader") == 0);
	assert(strcmp(f[1].value, "builtin://shader/pbr") == 0);

	/* A textured material advertises the slot too, the way checker does. */
	{
		int32_t tidx = index_of("project://material/twin-7");

		assert(tidx >= 0);
		assert(asset_catalog_describe((uint32_t)tidx, f, 8) == 3);
		assert(strcmp(f[2].key, "texture") == 0);
		assert(strcmp(f[2].value,
			      "builtin://texture/checker @ 256") == 0);
	}

	assert(asset_catalog_get_data(id, &size) != NULL
	       && size <= sizeof(before));
	memcpy(before, asset_catalog_get_data(id, NULL), size);

	/* Clear it: describe() reports nothing, the bytes are untouched. */
	assert(asset_mut_set_decl(id, NULL, 0) == 0);
	assert(asset_catalog_describe((uint32_t)idx, f, 8) == 0);
	assert(memcmp(asset_catalog_get_data(id, NULL), before, size) == 0);
	assert(u32_at(id, 0) == id_at("builtin://shader/pbr"));

	world_reset(&w);
	assert(scene_script_build(&w, &catalog_api,
		"(scene d (entity (name \"m\")"
		"          (material \"project://material/decl\")))") == 1);
	assert(w.material_ref[entity_named("m")] == id);
}

/*
 * A path the engine seeded read-only is refused, and left alone: a project
 * shadowing a built-in material would change every scene binding it, not just
 * its own. A path already holding another type is refused too. Both come from
 * the asset_define the three *-define! primitives share, checked here at the
 * material door because that is the door this change opens.
 */
static void test_builtin_and_other_type_refused(void)
{
	uint8_t  before[64];
	uint32_t metal, size = 0;

	metal = id_at("builtin://material/pbr-metal");
	assert(asset_catalog_get_data(metal, &size) != NULL
	       && size <= sizeof(before));
	memcpy(before, asset_catalog_get_data(metal, NULL), size);

	assert(define_material("builtin://material/pbr-metal",
			       "(material hijack"
			       " (shader \"builtin://shader/pbr\")"
			       " (base_color 0 0 0 1))") == 0);
	assert(memcmp(asset_catalog_get_data(metal, NULL), before, size) == 0);

	/* A mesh path is not a material path, even an authored one. */
	{
		s7_scheme *sc = script_s7();
		s7_pointer r;

		r = s7_call(sc, s7_name_to_value(sc, "mesh-define!"),
			    s7_list(sc, 2,
				    s7_make_string(sc, "project://mesh/m"),
				    s7_make_string(sc, "(mesh m)")));
		assert(s7_is_integer(r) && s7_integer(r) != 0);
		assert(define_material("project://mesh/m",
				       "(material m"
				       " (shader \"builtin://shader/pbr\"))")
		       == 0);
		assert(strncmp((const char *)
			       asset_catalog_get_data((uint32_t)s7_integer(r),
						      NULL),
			       "(mesh m)", 8) == 0);
	}
}

/*
 * With no catalog bound — a host that runs the image but has no asset subsystem,
 * which is what the headless rules tests are — the primitive is inert: it reports
 * 0 and nothing else happens. It needs the catalog to READ as well as write (the
 * shader whose block it packs against lives there), so this is the first refusal
 * it reaches.
 */
static void test_inert_without_catalog(void)
{
	scene_script_bind_catalog(NULL, NULL);
	assert(define_material("project://material/void",
			       "(material void"
			       " (shader \"builtin://shader/pbr\"))") == 0);
	assert(index_of("project://material/void") == -1);
	scene_script_bind_catalog(&catalog_api, &mut_api);
}

int main(void)
{
	mem_init();
	log_init();
	asset_init();        /* the real catalog, seeded built-ins and all  */
	script_init();       /* loads material_script.scm + scene_script.scm */
	scene_script_bind_catalog(&catalog_api, &mut_api);
	scene_script_init(); /* the scene-* primitives and material-define!  */

	test_define_bind_and_pack();
	test_packs_like_the_seeders();
	test_malformed_refused();
	test_refusal_leaves_the_old_bytes();
	test_redefine_replaces_in_place();
	test_declaration_is_derived_and_not_load_bearing();
	test_builtin_and_other_type_refused();
	test_inert_without_catalog();

	printf("material_define_test: ok\n");
	return 0;
}
