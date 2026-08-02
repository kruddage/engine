/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * kgui_accessors_test — the krudd-* accessors against fake subsystem apis.
 *
 * The panels these feed are drawn in a browser, so this harness stands in for
 * one: it builds a subsystem manager whose static table publishes fake log /
 * stats / scene / asset / asset_mut / edit vtables over fixtures held here,
 * registers the accessors against the real s7 image, and asserts the Scheme
 * shape each one hands back — the shape kruddgui.scm reads by position, and the
 * one kgui_scene_test / kgui_assets_test stub out on the other side of the seam.
 *
 * The parameter accessors are not faked: they run the real script.h
 * introspection over real shader / mesh / script sources, so a change in the
 * source dialect that moved an offset would fail here rather than in the browser.
 */

#include "kgui_accessors.h"

#include "s7.h"
#include "script.h"

#include "asset_api.h"
#include "edit_api.h"
#include "entity_api.h"
#include "log_api.h"
#include "stats_api.h"
#include "subsystem.h"
#include "subsystem_manager.h"

#include "math_types.h"
#include "world.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* fixtures                                                            */
/* ------------------------------------------------------------------ */

/* A shader with a Material block: a vec4 colour then a float, std140-packed. */
static const char *SHADER_SRC =
	"(shader m (uniforms (Material (block 1) (layout std140)"
	"  (base_color vec4 (edit color))"
	"  (roughness  float (edit range 0 1))))"
	"  (fragment (set c (vec4 (* base_color roughness) 1.0))))";

/* A mesh script with one declared param, so the mesh param path has something. */
static const char *MESH_SRC =
	"(mesh box (params (size float (default 2.0) (edit range 0.1 3.0)))"
	"  (generate () (cons '() '())))";

/* An entity script with one declared param. */
static const char *SCRIPT_SRC =
	"(script spin (params (speed float (default 90) (edit range 0 720)))"
	"  (on-tick (self t) #f))";

static const char *TEXT_SRC = "# Title\n\nSome **bold** text.\n";

/* The material asset's bytes: shader-ref header, then the std140 block. */
static uint8_t g_material_bytes[4 + 32];
static uint32_t g_material_size;

enum {
	AID_SHADER   = 1,  /* read-only built-in  */
	AID_MESH     = 10, /* authored            */
	AID_SCRIPT   = 11, /* authored            */
	AID_MATERIAL = 12, /* authored            */
	AID_TEXT     = 13, /* authored            */
	AID_COUNT    = 5
};

struct fake_asset {
	uint32_t    id;
	const char *path;
	int32_t     type;
	int32_t     kind;
	const void *bytes;
	uint32_t    size;
};

static struct fake_asset g_assets[AID_COUNT];

static struct world g_world;

/* What the mutating fakes recorded, so a test can assert the write happened. */
static char     g_last_call[256];
static uint32_t g_last_set_id;
static uint8_t  g_last_set_bytes[256];
static uint32_t g_last_set_len;
static int32_t  g_last_entity_id;
static uint8_t  g_last_entity_bytes[256];
static uint32_t g_last_entity_len;

static int g_can_undo = 1;
static int g_can_redo;
static int g_undo_calls;

/* ------------------------------------------------------------------ */
/* fake apis                                                           */
/* ------------------------------------------------------------------ */

static uint32_t fake_log_history(struct log_message *out, uint32_t max)
{
	uint32_t n = 2;

	if (max < n)
		n = max;
	if (n > 0) {
		out[0].level = 1;
		snprintf(out[0].text, sizeof(out[0].text), "first");
	}
	if (n > 1) {
		out[1].level = 3;
		snprintf(out[1].text, sizeof(out[1].text), "second");
	}
	return n;
}

static const struct log_api g_log = { NULL, fake_log_history };

static struct stats_api g_stats;

static const struct world *fake_get_world(void)
{
	return &g_world;
}

static int32_t fake_get_selected(void)
{
	return g_world.selected;
}

static void fake_set_selected(int32_t id)
{
	g_world.selected = id;
	snprintf(g_last_call, sizeof(g_last_call), "select %d", id);
}

static int32_t fake_create_entity(int32_t parent, const struct transform *t,
				  uint32_t mask, uint32_t render_ref)
{
	(void)parent;
	(void)t;
	(void)mask;
	(void)render_ref;
	snprintf(g_last_call, sizeof(g_last_call), "create");
	return 7;
}

static void fake_destroy_entity(int32_t id)
{
	snprintf(g_last_call, sizeof(g_last_call), "destroy %d", id);
}

static void fake_set_name(int32_t id, const char *name)
{
	snprintf(g_last_call, sizeof(g_last_call), "name %d %s", id,
		 name ? name : "(null)");
}

static void fake_set_transform(int32_t id, const struct transform *t)
{
	snprintf(g_last_call, sizeof(g_last_call), "xform %d %.2f %.2f %.2f", id,
		 (double)t->position[0], (double)t->rotation[3],
		 (double)t->scale[1]);
}

static void fake_set_render_ref(int32_t id, uint32_t ref)
{
	snprintf(g_last_call, sizeof(g_last_call), "render %d %u", id, ref);
}

static void fake_set_material_ref(int32_t id, uint32_t ref)
{
	snprintf(g_last_call, sizeof(g_last_call), "material %d %u", id, ref);
}

static void fake_set_script_ref(int32_t id, uint32_t ref)
{
	snprintf(g_last_call, sizeof(g_last_call), "script %d %u", id, ref);
}

static void record_entity_params(int32_t id, const uint8_t *b, uint32_t len)
{
	g_last_entity_id  = id;
	g_last_entity_len = len;
	if (len > sizeof(g_last_entity_bytes))
		len = sizeof(g_last_entity_bytes);
	if (b)
		memcpy(g_last_entity_bytes, b, len);
}

static void fake_set_script_params(int32_t id, const uint8_t *b, uint32_t len)
{
	snprintf(g_last_call, sizeof(g_last_call), "script-params");
	record_entity_params(id, b, len);
}

static void fake_set_material_params(int32_t id, const uint8_t *b, uint32_t len)
{
	snprintf(g_last_call, sizeof(g_last_call), "material-params");
	record_entity_params(id, b, len);
}

static void fake_set_mesh_params(int32_t id, const uint8_t *b, uint32_t len)
{
	snprintf(g_last_call, sizeof(g_last_call), "mesh-params");
	record_entity_params(id, b, len);
}

static void fake_set_texture_params(int32_t id, const uint8_t *b, uint32_t len)
{
	snprintf(g_last_call, sizeof(g_last_call), "texture-params");
	record_entity_params(id, b, len);
}

static struct entity_api g_scene;

static uint32_t fake_asset_count(void)
{
	return AID_COUNT;
}

static int32_t fake_asset_info(uint32_t i, struct asset_info *out)
{
	if (i >= AID_COUNT || !out)
		return -1;
	memset(out, 0, sizeof(*out));
	out->id        = g_assets[i].id;
	out->path      = g_assets[i].path;
	out->type      = g_assets[i].type;
	out->kind      = g_assets[i].kind;
	out->state     = 1;
	out->size      = g_assets[i].size;
	out->refs      = 1;
	out->read_only = g_assets[i].kind == ASSET_KIND_PRIMITIVE;
	out->origin    = g_assets[i].kind == ASSET_KIND_PRIMITIVE
				 ? ASSET_ORIGIN_FETCHED
				 : ASSET_ORIGIN_AUTHORED;
	return 0;
}

static int32_t fake_asset_find(uint32_t id, struct asset_info *out)
{
	uint32_t i;

	for (i = 0; i < AID_COUNT; i++) {
		if (g_assets[i].id == id)
			return fake_asset_info(i, out);
	}
	return -1;
}

static const void *fake_asset_get_data(uint32_t id, uint32_t *out_size)
{
	uint32_t i;

	for (i = 0; i < AID_COUNT; i++) {
		if (g_assets[i].id != id)
			continue;
		if (out_size)
			*out_size = g_assets[i].size;
		return g_assets[i].bytes;
	}
	return NULL;
}

static const struct asset_api g_asset = { fake_asset_count, fake_asset_info,
					  NULL, fake_asset_find,
					  fake_asset_get_data };

static uint32_t fake_mut_create(const char *path, int32_t type,
				const void *bytes, uint32_t size)
{
	(void)bytes;
	snprintf(g_last_call, sizeof(g_last_call), "create %s type=%d size=%u",
		 path ? path : "(null)", type, size);
	return 99;
}

static int32_t fake_mut_set_data(uint32_t id, const void *bytes, uint32_t size)
{
	g_last_set_id  = id;
	g_last_set_len = size;
	if (size > sizeof(g_last_set_bytes))
		size = sizeof(g_last_set_bytes);
	if (bytes)
		memcpy(g_last_set_bytes, bytes, size);
	snprintf(g_last_call, sizeof(g_last_call), "set-data %u %u", id,
		 g_last_set_len);
	return 0;
}

static int32_t fake_mut_destroy(uint32_t id)
{
	snprintf(g_last_call, sizeof(g_last_call), "destroy-asset %u", id);
	return 0;
}

static const struct asset_mut_api g_asset_mut = { fake_mut_create,
						  fake_mut_set_data,
						  fake_mut_destroy, NULL,
						  NULL };

static int32_t fake_can_undo(void)
{
	return g_can_undo;
}

static int32_t fake_can_redo(void)
{
	return g_can_redo;
}

static int32_t fake_undo(void)
{
	g_undo_calls++;
	return 1;
}

static struct edit_api g_edit;

/* ------------------------------------------------------------------ */
/* harness                                                             */
/* ------------------------------------------------------------------ */

static struct subsystem        g_table[8];
static struct subsystem_manager g_mgr;

static void seed_world(void)
{
	memset(&g_world, 0, sizeof(g_world));
	g_world.count    = 3;
	g_world.selected = 1;

	/* 0: "Root", no components beyond its name. */
	g_world.alive[0]    = 1;
	g_world.parent[0]   = WORLD_NO_PARENT;
	g_world.mask[0]     = COMPONENT_NAME;
	g_world.name_off[0] = 0;
	memcpy(g_world.names, "Root", 5);

	/* 1: "Cube" under root, with a mesh, a material and a script bound. */
	g_world.alive[1]        = 1;
	g_world.parent[1]       = 0;
	g_world.mask[1]         = COMPONENT_NAME | COMPONENT_RENDER |
			  COMPONENT_MATERIAL | COMPONENT_SCRIPT;
	g_world.name_off[1]     = 5;
	memcpy(g_world.names + 5, "Cube", 5);
	g_world.render_ref[1]   = AID_MESH;
	g_world.material_ref[1] = AID_MATERIAL;
	g_world.script_ref[1]   = AID_SCRIPT;
	g_world.local[1].position[0] = 1.0f;
	g_world.local[1].position[1] = 2.0f;
	g_world.local[1].position[2] = 3.0f;
	g_world.local[1].rotation[3] = 1.0f;
	g_world.local[1].scale[0]    = 1.0f;
	g_world.local[1].scale[1]    = 1.0f;
	g_world.local[1].scale[2]    = 1.0f;

	/* 2: tombstoned — must never appear in the entity list. */
	g_world.alive[2]  = 0;
	g_world.parent[2] = WORLD_NO_PARENT;
}

static void seed_assets(void)
{
	float    color[4] = { 0.9f, 0.5f, 0.1f, 1.0f };
	float    rough    = 0.25f;
	uint32_t shader   = AID_SHADER;

	memcpy(g_material_bytes, &shader, sizeof(shader));
	memcpy(g_material_bytes + 4, color, sizeof(color));
	memcpy(g_material_bytes + 20, &rough, sizeof(rough));
	g_material_size = 4 + 20;

	g_assets[0] = (struct fake_asset){ AID_SHADER, "builtin://shader/m",
					   ASSET_TYPE_SHADER,
					   ASSET_KIND_PRIMITIVE, SHADER_SRC,
					   (uint32_t)strlen(SHADER_SRC) };
	g_assets[1] = (struct fake_asset){ AID_MESH, "mesh/box",
					   ASSET_TYPE_MESH, ASSET_KIND_NORMAL,
					   MESH_SRC,
					   (uint32_t)strlen(MESH_SRC) };
	g_assets[2] = (struct fake_asset){ AID_SCRIPT, "script/spin",
					   ASSET_TYPE_SCRIPT, ASSET_KIND_NORMAL,
					   SCRIPT_SRC,
					   (uint32_t)strlen(SCRIPT_SRC) };
	g_assets[3] = (struct fake_asset){ AID_MATERIAL, "mat/brass",
					   ASSET_TYPE_MATERIAL,
					   ASSET_KIND_NORMAL, g_material_bytes,
					   g_material_size };
	g_assets[4] = (struct fake_asset){ AID_TEXT, "notes/readme",
					   ASSET_TYPE_TEXT, ASSET_KIND_NORMAL,
					   TEXT_SRC,
					   (uint32_t)strlen(TEXT_SRC) };
}

static s7_scheme *setup(void)
{
	s7_scheme *sc = script_s7();
	int        i  = 0;

	assert(sc);

	g_stats.last_frame_ms          = 16.5f;
	g_stats.fps_avg                = 60.5f;
	g_stats.frame_count            = 1234;
	g_stats.init_ms                = 40.0f;
	g_stats.first_frame_ms         = 55.0f;
	g_stats.page_to_first_frame_ms = 900.0f;
	g_stats.phase_count            = 2;
	g_stats.phases[0].name         = "script_init";
	g_stats.phases[0].ms           = 12.0f;
	g_stats.phases[1].name         = "renderer";
	g_stats.phases[1].ms           = 8.0f;

	memset(&g_scene, 0, sizeof(g_scene));
	g_scene.get_world           = fake_get_world;
	g_scene.get_selected        = fake_get_selected;
	g_scene.set_selected        = fake_set_selected;
	g_scene.create_entity       = fake_create_entity;
	g_scene.destroy_entity      = fake_destroy_entity;
	g_scene.set_name            = fake_set_name;
	g_scene.set_transform       = fake_set_transform;
	g_scene.set_render_ref      = fake_set_render_ref;
	g_scene.set_material_ref    = fake_set_material_ref;
	g_scene.set_script_ref      = fake_set_script_ref;
	g_scene.set_script_params   = fake_set_script_params;
	g_scene.set_material_params = fake_set_material_params;
	g_scene.set_mesh_params     = fake_set_mesh_params;
	g_scene.set_texture_params  = fake_set_texture_params;

	memset(&g_edit, 0, sizeof(g_edit));
	g_edit.can_undo = fake_can_undo;
	g_edit.can_redo = fake_can_redo;
	g_edit.undo     = fake_undo;

	seed_world();
	seed_assets();

	memset(g_table, 0, sizeof(g_table));
	g_table[i].name = "log";       g_table[i++].api = &g_log;
	g_table[i].name = "stats";     g_table[i++].api = &g_stats;
	g_table[i].name = "scene";     g_table[i++].api = &g_scene;
	g_table[i].name = "asset";     g_table[i++].api = &g_asset;
	g_table[i].name = "asset_mut"; g_table[i++].api = &g_asset_mut;
	g_table[i].name = "edit";      g_table[i++].api = &g_edit;
	g_table[i].tick = NULL;        /* the terminator's name stays NULL */

	memset(&g_mgr, 0, sizeof(g_mgr));
	g_mgr.static_table = g_table;

	kgui_accessors_register(sc, &g_mgr);
	return sc;
}

static s7_scheme *g_sc;

static int truth(const char *src)
{
	return s7_eval_c_string(g_sc, src) == s7_t(g_sc);
}

static double number(const char *src)
{
	return s7_number_to_real(g_sc, s7_eval_c_string(g_sc, src));
}

static const char *text(const char *src)
{
	s7_pointer p = s7_eval_c_string(g_sc, src);

	return s7_is_string(p) ? s7_string(p) : "";
}

#define CHECK(name, expr)                                                     \
	do {                                                                  \
		if (expr) {                                                   \
			printf("PASS: %s\n", name);                           \
			passed++;                                             \
		} else {                                                      \
			printf("FAIL: %s\n", name);                           \
			failed++;                                             \
		}                                                             \
		total++;                                                      \
	} while (0)

int main(void)
{
	int total = 0, passed = 0, failed = 0;

	g_sc = setup();

	/*
	 * The accessors hand back C floats widened to Scheme reals, so a
	 * literal 0.9 never compares equal to (double)0.9f. Every value
	 * assertion below goes through these.
	 */
	s7_eval_c_string(g_sc,
			 "(begin"
			 " (define (close? a b) (< (abs (- a b)) 1e-5))"
			 " (define (close-list? xs ys)"
			 "  (cond ((and (null? xs) (null? ys)) #t)"
			 "        ((or (null? xs) (null? ys)) #f)"
			 "        ((close? (car xs) (car ys))"
			 "         (close-list? (cdr xs) (cdr ys)))"
			 "        (else #f))))");

	/* ---- log / stats / subsystems ---- */
	CHECK("log history is oldest-first (level . text) pairs",
	      truth("(equal? (krudd-log-history) '((1 . \"first\") "
		    "(3 . \"second\")))"));
	CHECK("stats reports (fps frame-ms frame-count)",
	      number("(caddr (krudd-stats))") == 1234.0 &&
		      number("(car (krudd-stats))") > 60.0);
	CHECK("startup carries the three bookends then the phases",
	      number("(car (krudd-startup))") == 40.0 &&
		      number("(caddr (krudd-startup))") == 900.0 &&
		      truth("(equal? (car (cdddr (krudd-startup))) "
			    "'(\"script_init\" . 12.0))"));
	CHECK("subsystems rows are (name api? tick? size)",
	      truth("(equal? (car (krudd-subsystems)) '(\"log\" #t #f 0))") &&
		      number("(length (krudd-subsystems))") == 6.0);

	/* ---- markdown ---- */
	CHECK("md-parse splits a heading and a bold run",
	      number("(length (krudd-md-parse \"# Title\n\ntext\"))") == 2.0 &&
		      number("(car (car (krudd-md-parse \"# T\")))") == 1.0);
	CHECK("md-parse of a non-string is ()",
	      truth("(null? (krudd-md-parse \"\"))"));

	/* ---- undo / redo and the tool mode ---- */
	CHECK("can-undo / can-redo follow the edit service",
	      truth("(krudd-can-undo)") && truth("(not (krudd-can-redo))"));
	CHECK("undo drives the service", truth("(krudd-undo)") &&
					 g_undo_calls == 1);
	CHECK("redo with no redo hook is #f", truth("(not (krudd-redo))"));
	CHECK("gizmo mode round-trips and rejects out-of-range",
	      (s7_eval_c_string(g_sc, "(krudd-set-gizmo-mode 2)"),
	       number("(krudd-gizmo-mode)") == 2.0) &&
		      (s7_eval_c_string(g_sc, "(krudd-set-gizmo-mode 9)"),
		       number("(krudd-gizmo-mode)") == 2.0));

	/* ---- scene ---- */
	CHECK("world caps report both services live",
	      truth("(equal? (krudd-world-caps) '(#t #t))"));
	CHECK("entity list skips the tombstone",
	      truth("(equal? (krudd-world-entities) "
		    "'((0 . \"Root\") (1 . \"Cube\")))"));
	CHECK("selection reads through", number("(krudd-selected)") == 1.0);
	CHECK("inspect returns #f for a dead id",
	      truth("(not (krudd-entity-inspect 2))") &&
		      truth("(not (krudd-entity-inspect 99))"));
	CHECK("inspect tuple carries name, transform and parent",
	      truth("(equal? (list-ref (krudd-entity-inspect 1) 0) \"Cube\")") &&
		      truth("(equal? (list-ref (krudd-entity-inspect 1) 1) "
			    "'(1.0 2.0 3.0))") &&
		      truth("(equal? (list-ref (krudd-entity-inspect 1) 4) "
			    "'(0 . \"Root\"))"));
	CHECK("inspect tuple carries the three bindings by position",
	      truth("(list-ref (krudd-entity-inspect 1) 6)") &&
		      number("(list-ref (krudd-entity-inspect 1) 8)") ==
			      (double)AID_MESH &&
		      number("(list-ref (krudd-entity-inspect 1) 9)") ==
			      (double)AID_MATERIAL &&
		      number("(list-ref (krudd-entity-inspect 1) 11)") ==
			      (double)AID_SCRIPT);
	CHECK("a root entity reports #f for its parent",
	      truth("(not (list-ref (krudd-entity-inspect 0) 4))"));
	CHECK("set-transform passes position, quaternion and scale through",
	      (s7_eval_c_string(g_sc, "(krudd-entity-set-transform 1 '(4 5 6) "
				      "'(0 0 0 1) '(2 3 4))"),
	       strcmp(g_last_call, "xform 1 4.00 1.00 3.00") == 0));
	CHECK("set-name reaches the service",
	      (s7_eval_c_string(g_sc, "(krudd-entity-set-name 1 \"Box\")"),
	       strcmp(g_last_call, "name 1 Box") == 0));
	CHECK("the three binding setters reach the service",
	      (s7_eval_c_string(g_sc, "(krudd-entity-set-render-ref 1 10)"),
	       strcmp(g_last_call, "render 1 10") == 0));

	/* ---- declared parameters (the real script.h introspection) ---- */
	CHECK("shader Material block reports its two fields in order",
	      number("(length (krudd-shader-material-params 1))") == 2.0 &&
		      truth("(equal? (car (car (krudd-shader-material-params "
			    "1))) \"base_color\")") &&
		      truth("(equal? (list-ref (car (krudd-shader-material-"
			    "params 1)) 5) \"color\")"));
	CHECK("the second field is the range-edited float at offset 16",
	      number("(list-ref (cadr (krudd-shader-material-params 1)) 2)") ==
			      16.0 &&
		      number("(list-ref (cadr (krudd-shader-material-params 1))"
			     " 4)") == 1.0);
	CHECK("mesh params read the mesh source's clause",
	      number("(length (krudd-mesh-params 10))") == 1.0 &&
		      truth("(equal? (car (car (krudd-mesh-params 10))) "
			    "\"size\")"));
	CHECK("script params read the script source's clause",
	      number("(length (krudd-script-params 11))") == 1.0);
	CHECK("params of a source-less id are ()",
	      truth("(null? (krudd-mesh-params 999))"));

	/* ---- material values ---- */
	CHECK("material values decode the std140 block",
	      truth("(close-list? (car (krudd-material-values 12 1)) "
		    "'(0.9 0.5 0.1 1.0))"));
	CHECK("the trailing float decodes at its own offset",
	      number("(car (cadr (krudd-material-values 12 1)))") > 0.24 &&
		      number("(car (cadr (krudd-material-values 12 1)))") <
			      0.26);
	CHECK("shader-ref reads the material's header",
	      number("(krudd-asset-shader-ref 12)") == 1.0);
	CHECK("a material with no texture trailer reports ()",
	      truth("(null? (krudd-material-texture 12))"));

	/* ---- per-entity overrides ---- */
	CHECK("an entity with no override reports the asset's values",
	      truth("(close-list? (car (krudd-entity-material-values 1 12 1)) "
		    "'(0.9 0.5 0.1 1.0))"));
	CHECK("mesh values fall back to the authored default",
	      truth("(close-list? (car (krudd-entity-mesh-values 1 10)) "
		    "'(2.0))"));
	CHECK("saving entity params packs against the declared layout",
	      (s7_eval_c_string(g_sc, "(krudd-entity-save-mesh-params 1 10 "
				      "'((1.5)))"),
	       g_last_entity_id == 1 && g_last_entity_len == 4));

	/* ---- the asset catalog ---- */
	CHECK("assets split into engine and project groups",
	      number("(length (car (krudd-assets)))") == 1.0 &&
		      number("(length (cadr (krudd-assets)))") == 4.0);
	CHECK("an asset row is (id path type kind state size refs)",
	      number("(car (car (car (krudd-assets))))") == 1.0 &&
		      truth("(equal? (cadr (car (car (krudd-assets)))) "
			    "\"builtin://shader/m\")"));
	CHECK("asset-info is the 8-tuple, #f for an unknown id",
	      number("(length (krudd-asset-info 10))") == 8.0 &&
		      truth("(not (krudd-asset-info 777))"));
	CHECK("asset-find resolves a path and misses cleanly",
	      truth("(equal? (krudd-asset-find 10) \"mesh/box\")") &&
		      truth("(not (krudd-asset-find 777))"));
	CHECK("asset-data hands back the source bytes",
	      strstr(text("(krudd-asset-data 11)"), "on-tick") != NULL);
	CHECK("the type filters return ((id . path) ...)",
	      truth("(equal? (krudd-mesh-assets) '((10 . \"mesh/box\")))") &&
		      truth("(equal? (krudd-shader-assets) "
			    "'((1 . \"builtin://shader/m\")))"));
	CHECK("asset-mut? follows the mutation service",
	      truth("(krudd-asset-mut?)"));

	/* ---- authoring ---- */
	CHECK("create seeds a compiling source of the right type",
	      (number("(krudd-asset-create-mesh \"m2\")") == 99.0) &&
		      strstr(g_last_call, "type=1") != NULL &&
		      strstr(g_last_call, "create m2") != NULL);
	CHECK("a created material is the bare shader-ref header",
	      (number("(krudd-asset-create-material \"m3\")") == 99.0) &&
		      strstr(g_last_call, "size=4") != NULL);
	CHECK("save-text writes the buffer with no compile gate",
	      (s7_eval_c_string(g_sc, "(krudd-asset-save-text 13 \"hello\")"),
	       g_last_set_id == 13 && g_last_set_len == 5));
	CHECK("save-mesh refuses an unbalanced source and a wrong head",
	      truth("(not (krudd-asset-save-mesh 10 \"(not a mesh\"))") &&
		      truth("(not (krudd-asset-save-mesh 10 \"(mesh b\"))") &&
		      truth("(not (krudd-asset-save-mesh 10 \"(meshy b)\"))"));
	CHECK("save-mesh accepts a good source",
	      truth("(krudd-asset-save-mesh 10 \"(mesh b (generate () (cons "
		    "'() '())))\")"));
	CHECK("save-material packs header, block and values",
	      (s7_eval_c_string(g_sc, "(krudd-asset-save-material 12 1 "
				      "'((0.1 0.2 0.3 0.4) (0.5)))"),
	       g_last_set_id == 12 && g_last_set_len == 36));
	CHECK("clone-material returns the new id",
	      number("(krudd-asset-clone-material \"copy\" 1 "
		     "'((0.1 0.2 0.3 0.4) (0.5)))") == 99.0);
	CHECK("delete reaches the mutation service",
	      (s7_eval_c_string(g_sc, "(krudd-asset-delete 13)"),
	       strcmp(g_last_call, "destroy-asset 13") == 0));

	/* ---- derived source facts ---- */
	CHECK("script hooks are derived from the source's clauses",
	      strcmp(text("(krudd-script-hooks (krudd-asset-data 11))"),
		     "on-tick") == 0);
	CHECK("a source with no hooks reports the empty string",
	      strcmp(text("(krudd-script-hooks \"(script s)\")"), "") == 0);
	CHECK("bakes report 0 with no preview service",
	      number("(krudd-mesh-bake 10 12 128)") == 0.0 &&
		      number("(krudd-texture-bake 10 '() 128)") == 0.0);

	/* ---- degradation with no services at all ---- */
	kgui_accessors_register(g_sc, NULL);
	CHECK("every accessor degrades rather than faulting with no manager",
	      truth("(not (krudd-stats))") && truth("(not (krudd-assets))") &&
		      truth("(null? (krudd-world-entities))") &&
		      truth("(not (krudd-entity-inspect 1))") &&
		      truth("(equal? (krudd-world-caps) '(#f #f))") &&
		      truth("(not (krudd-can-undo))"));

	printf("\n%d/%d kgui accessor tests passed\n", passed, total);
	return failed == 0 ? 0 : 1;
}
