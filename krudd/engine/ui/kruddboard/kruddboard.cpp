/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * kruddboard — engine debug overlay plugin.
 *
 * Every panel that was once an ImGui tab of this overlay — the Log console, the
 * KRUDD tab (frame stats, startup profile, subsystems), the World/Scene
 * inspector and the Assets browser — is now a standalone kruddgui console drawn
 * in kruddgui.scm (#491/#492). The board's own ImGui window, header and tab bar
 * are gone with them, and the header's live controls (undo/redo, play/pause)
 * moved onto kruddgui's top toolbar off the accessors registered here.
 *
 * What remains in C here: the shared krudd-* accessors the kruddgui panels read
 * (registered against the s7 interpreter), and the viewport-space editor tools —
 * the transform gizmo and click-to-pick (draw_viewport_tools) — which now draw on
 * kruddgui's own batch and read its pointer through the overlay seam (kruddgui_api,
 * #492, feeding #487). No ImGui remains in kruddboard; drag-to-spawn, which rode
 * ImGui's drag/drop, is retired. Toggle the viewport tools with backtick (`).
 */

extern "C" {
#include "subsystem.h"
#include "subsystem_manager.h"
#include "log_api.h"
#include "log_level.h"
#include "stats_api.h"
#include "kruddgui_api.h"
#include "asset_api.h"
#include "entity_api.h"
#include "edit_api.h"
#include "camera_api.h"
#include "preview_api.h"
#include "mesh.h"
#include "memory_api.h"
#ifdef __EMSCRIPTEN__
#include "backend_api.h"
#include "script.h"
#include "mesh_script.h"
#include "texture_script.h"
#include "renderer.h"		/* gpu_api — texture bakes ride the device seam */
#endif
}

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5.h>
#include <string.h>
#include <math.h>
#include "md_parse.h"
#include "s7.h"			/* self-guards for C++ linkage */

/*
 * Soft-keyboard capture flag (plugin_abi.c): true while a kruddgui field owns
 * text input. on_keydown checks it to leave a focused field's own edit-undo
 * alone, the kruddgui replacement for ImGui's WantTextInput.
 */
extern "C" int krudd_text_input_capture(void);
#endif

#include <cstdio>
#include <cfloat>

static const struct log_api           *g_log;
static const struct stats_api         *g_stats;
static const struct asset_api         *g_asset_api;
static const struct memory_api        *g_mem; /* for click-to-pick mesh gen */
static const struct subsystem_manager *g_mgr;
static int                             g_visible = 1;
static int                             g_panels_registered;
static uint32_t                        asset_id_by_path(const char *path);
static const struct entity_api        *g_entity_api;
static int32_t                         g_entity_sel = -1; /* -1 = none */
static const struct edit_api          *g_edit_api;  /* NULL = no history */
static const struct camera_api        *g_camera_api; /* NULL = no viewport gizmo */
static const struct preview_api       *g_preview_api; /* NULL = no mesh preview */
static const struct kruddgui_api      *g_kgui; /* the overlay seam; NULL until up */

/* Transform gizmo mode, shared between the viewport handles and the World tab. */
enum gizmo_mode {
	GIZMO_MOVE,
	GIZMO_ROTATE,
	GIZMO_SCALE,
};
static enum gizmo_mode g_gizmo_mode = GIZMO_MOVE;

#ifdef __EMSCRIPTEN__
static const struct asset_mut_api     *g_asset_mut;
static const struct backend_api       *g_backend;
#endif

/* ------------------------------------------------------------------ */
/* Visibility toggle                                                   */
/* ------------------------------------------------------------------ */

#ifdef __EMSCRIPTEN__
static EM_BOOL on_keydown(int /*type*/, const EmscriptenKeyboardEvent *e,
			  void * /*ud*/)
{
	bool ctrl;

	if (strcmp(e->code, "Backquote") == 0) {
		g_visible = !g_visible;
		return EM_TRUE;
	}

	/*
	 * Global undo/redo: Ctrl+Z undoes, Ctrl+Y / Ctrl+Shift+Z redo (Cmd on
	 * mac). Skip entirely when a kruddgui field owns text input so a focused
	 * markdown/shader field keeps its own edit undo — the shortcut must never
	 * both edit text and pop the global stack in one press. No "edit" service
	 * (older engine build) means these are safe no-ops.
	 */
	if (!g_edit_api || krudd_text_input_capture())
		return EM_FALSE;

	ctrl = e->ctrlKey || e->metaKey;
	if (!ctrl)
		return EM_FALSE;

	if (strcmp(e->code, "KeyZ") == 0) {
		if (e->shiftKey)
			g_edit_api->redo();
		else
			g_edit_api->undo();
		return EM_TRUE;
	}
	if (strcmp(e->code, "KeyY") == 0) {
		g_edit_api->redo();
		return EM_TRUE;
	}
	return EM_FALSE;
}
#endif

/* ------------------------------------------------------------------ */
/* Scheme accessor host                                                */
/* ------------------------------------------------------------------ */

#ifdef __EMSCRIPTEN__
/*
 * The seam between the engine ABIs and the kruddgui panels authored in Scheme.
 * The krudd-* accessors below are registered against the shared s7 interpreter —
 * the same one the shader DSL and the runtime tick run in — and read at draw
 * time by the kruddgui consoles (kruddgui.scm): the frame stats and subsystem
 * table, the entity list and its undo-recording inspector, the asset browser and
 * editors, and the top toolbar's undo/redo + play/pause. Every panel's pixels
 * are now kruddgui's; C keeps only these data/command bindings over the engine.
 */

/*
 * The s7 callbacks below carry C language linkage to match the s7_function
 * pointer type they are registered as. They are the krudd-* accessors the
 * kruddgui panels read — data and command bindings over the engine ABIs, each
 * cheap enough to call once per drawn frame.
 */
extern "C" {

/* (krudd-stats) -> (fps frame-ms frame-count), or #f when stats are absent. */
static s7_pointer sp_krudd_stats(s7_scheme *sc, s7_pointer args)
{
	(void)args;
	if (!g_stats)
		return s7_f(sc);
	return s7_list(sc, 3,
		       s7_make_real(sc, (s7_double)g_stats->fps_avg),
		       s7_make_real(sc, (s7_double)g_stats->last_frame_ms),
		       s7_make_integer(sc, (s7_int)g_stats->frame_count));
}

/*
 * (krudd-startup) -> (init-ms first-frame-ms page-first-ms (name . ms) ...), or
 * #f when the stats subsystem is absent. init-ms is the whole engine_init wall
 * time, first-frame-ms is boot-start to the first tick, and page-first-ms is
 * the full page-navigation-to-first-frame wall clock (download and WASM compile
 * included — the span the boot-relative numbers miss). Each trailing pair is
 * one startup phase in boot order; the phases are consed tail-first so they come
 * back oldest-first, matching the order they ran.
 */
static s7_pointer sp_krudd_startup(s7_scheme *sc, s7_pointer args)
{
	s7_pointer phases;
	int        i;

	(void)args;
	if (!g_stats)
		return s7_f(sc);
	phases = s7_nil(sc);
	for (i = (int)g_stats->phase_count - 1; i >= 0; i--)
		phases = s7_cons(sc,
			s7_cons(sc,
				s7_make_string(sc, g_stats->phases[i].name),
				s7_make_real(sc, (s7_double)g_stats->phases[i].ms)),
			phases);
	return s7_cons(sc,
		s7_make_real(sc, (s7_double)g_stats->init_ms),
		s7_cons(sc,
			s7_make_real(sc, (s7_double)g_stats->first_frame_ms),
			s7_cons(sc,
				s7_make_real(sc,
					(s7_double)g_stats->page_to_first_frame_ms),
				phases)));
}

/*
 * imgui-begin-child / imgui-end-child / imgui-set-scroll-here-y /
 * imgui-viewport-work-height were the scroll-region primitives the ImGui Log tab
 * used; they were orphaned when the Log moved to the kruddgui console (#491) and
 * are removed here (#492), the first ImGui bindings retired as the strangler
 * migration removes their last consumer.
 */

/* One (name api? tick? wasm-size) row for the subsystems table. */
static s7_pointer subsystem_row(s7_scheme *sc, const struct subsystem *s)
{
	return s7_list(sc, 4,
		       s7_make_string(sc, s->name),
		       s7_make_boolean(sc, s->api  != NULL),
		       s7_make_boolean(sc, s->tick != NULL),
		       s7_make_integer(sc, (s7_int)s->wasm_size));
}

/*
 * (krudd-subsystems) -> a list of (name api? tick? wasm-size) rows in table
 * order (static table then dynamic), or #f when the manager is absent. api?
 * and tick? are booleans; wasm-size is an integer (0 = unknown).
 */
static s7_pointer sp_krudd_subsystems(s7_scheme *sc, s7_pointer args)
{
	s7_pointer out;
	int        i;

	(void)args;
	if (!g_mgr)
		return s7_f(sc);

	out = s7_nil(sc);
	for (i = 0; g_mgr->static_table[i].name; i++)
		out = s7_cons(sc, subsystem_row(sc, &g_mgr->static_table[i]),
			      out);
	for (i = 0; i < g_mgr->dynamic_count; i++)
		out = s7_cons(sc, subsystem_row(sc, &g_mgr->dynamic[i]), out);
	return s7_reverse(sc, out);
}

/*
 * (krudd-log-history) -> a list of (level . text) pairs oldest-first, or #f
 * when the log subsystem is absent. level is a log_level integer.
 */
static s7_pointer sp_krudd_log_history(s7_scheme *sc, s7_pointer args)
{
	static struct log_message msgs[LOG_HISTORY_CAP];
	uint32_t                  count, i;
	s7_pointer                out;

	(void)args;
	if (!g_log)
		return s7_f(sc);

	count = g_log->get_history(msgs, LOG_HISTORY_CAP);
	out   = s7_nil(sc);
	for (i = 0; i < count; i++) {
		uint32_t j = count - 1 - i;

		out = s7_cons(sc,
			      s7_cons(sc,
				      s7_make_integer(sc, (s7_int)msgs[j].level),
				      s7_make_string(sc, msgs[j].text)),
			      out);
	}
	return out;
}

/* ------------------------------------------------------------------ */
/* World-tab primitives (#401)                                         */
/* ------------------------------------------------------------------ */

/* Read n reals off a Scheme list into a float array (short lists pad nothing). */
static void read_reals(s7_scheme *sc, s7_pointer lst, float *out, int n)
{
	int i;

	for (i = 0; i < n && s7_is_pair(lst); i++) {
		out[i] = (float)s7_number_to_real(sc, s7_car(lst));
		lst    = s7_cdr(lst);
	}
}

/* Build a fresh (x y z ...) list of n reals from a float array. */
static s7_pointer real_list(s7_scheme *sc, const float *v, int n)
{
	s7_pointer out = s7_nil(sc);
	int        i;

	for (i = n - 1; i >= 0; i--)
		out = s7_cons(sc, s7_make_real(sc, (s7_double)v[i]), out);
	return out;
}

/*
 * The imgui-* Scheme drawing primitives that once backed the World and Assets
 * tabs — text, tables, combos, the collapsing headers, the input/colour widgets,
 * the mesh drag source — were pruned here when the last of those tabs moved onto
 * kruddgui's own widgets (#492). What survives are pure data helpers (read_reals
 * / real_list) and the krudd-* accessors below, which the kruddgui Scene console
 * reads; none of them touch ImGui.
 */

/* Selection id honouring the api / g_entity_sel fallback the World tab uses. */
static int32_t world_sel(void)
{
	return g_entity_api ? g_entity_api->get_selected() : g_entity_sel;
}

/*
 * (krudd-world-caps) -> (entity-api? asset-api?). The tab greys the create/edit
 * widgets when the scene api is absent and the binding combos when either the
 * scene or asset api is; these two booleans drive both.
 */
static s7_pointer sp_krudd_world_caps(s7_scheme *sc, s7_pointer args)
{
	(void)args;
	return s7_list(sc, 2,
		       s7_make_boolean(sc, g_entity_api != NULL),
		       s7_make_boolean(sc, g_asset_api != NULL));
}

/* (krudd-selected) -> the selected entity id, or -1 when nothing is selected. */
static s7_pointer sp_krudd_selected(s7_scheme *sc, s7_pointer args)
{
	(void)args;
	return s7_make_integer(sc, (s7_int)world_sel());
}

/*
 * (krudd-world-entities) -> a list of (id . name) for each live entity in id
 * order; name is a string or #f when the entity has no name. Returns #f when the
 * scene api or its world is absent, which the tab renders as "(no entities)".
 */
static s7_pointer sp_krudd_world_entities(s7_scheme *sc, s7_pointer args)
{
	const struct world *w = NULL;
	s7_pointer          out;
	uint32_t            i;

	(void)args;
	if (g_entity_api)
		w = g_entity_api->get_world();
	if (!w)
		return s7_f(sc);

	out = s7_nil(sc);
	for (i = 0; i < w->count; i++) {
		s7_pointer name;

		if (!w->alive[i])
			continue;
		if ((w->mask[i] & COMPONENT_NAME) &&
		    w->name_off[i] != SCENE_NO_NAME)
			name = s7_make_string(sc, w->names + w->name_off[i]);
		else
			name = s7_f(sc);
		out = s7_cons(sc,
			      s7_cons(sc, s7_make_integer(sc, (s7_int)i), name),
			      out);
	}
	return s7_reverse(sc, out);
}

/*
 * (krudd-entity-inspect id) -> the inspector bundle for a live entity, else #f:
 *   (name (px py pz) (rx ry rz rw) (sx sy sz) parent
 *    has-name? has-render? has-material? render-ref material-ref
 *    has-script? script-ref)
 * name is a string ("" when unnamed); parent is #f for a root or (pid . pname)
 * where pname is a string or #f; the refs are asset ids (0 = unbound).
 */
static s7_pointer sp_krudd_entity_inspect(s7_scheme *sc, s7_pointer args)
{
	const struct world     *w  = NULL;
	const struct transform *t;
	int32_t                 id = (int32_t)s7_integer(s7_car(args));
	uint32_t                e;
	s7_pointer              parent;
	const char             *name;

	if (g_entity_api)
		w = g_entity_api->get_world();
	if (!w || id < 0 || (uint32_t)id >= w->count ||
	    !w->alive[(uint32_t)id])
		return s7_f(sc);
	e = (uint32_t)id;
	t = &w->local[e];

	name = NULL;
	if ((w->mask[e] & COMPONENT_NAME) && w->name_off[e] != SCENE_NO_NAME)
		name = w->names + w->name_off[e];

	if (w->parent[e] < 0) {
		parent = s7_f(sc);
	} else {
		uint32_t    p  = (uint32_t)w->parent[e];
		const char *pn = NULL;

		if ((w->mask[p] & COMPONENT_NAME) &&
		    w->name_off[p] != SCENE_NO_NAME)
			pn = w->names + w->name_off[p];
		parent = s7_cons(sc, s7_make_integer(sc, (s7_int)p),
				 pn ? s7_make_string(sc, pn) : s7_f(sc));
	}

	return s7_list(sc, 12,
		s7_make_string(sc, name ? name : ""),
		real_list(sc, t->position, 3),
		real_list(sc, t->rotation, 4),
		real_list(sc, t->scale, 3),
		parent,
		s7_make_boolean(sc, (w->mask[e] & COMPONENT_NAME)     != 0),
		s7_make_boolean(sc, (w->mask[e] & COMPONENT_RENDER)   != 0),
		s7_make_boolean(sc, (w->mask[e] & COMPONENT_MATERIAL) != 0),
		s7_make_integer(sc, (s7_int)((w->mask[e] & COMPONENT_RENDER)
					     ? w->render_ref[e] : 0u)),
		s7_make_integer(sc, (s7_int)((w->mask[e] & COMPONENT_MATERIAL)
					     ? w->material_ref[e] : 0u)),
		s7_make_boolean(sc, (w->mask[e] & COMPONENT_SCRIPT)   != 0),
		s7_make_integer(sc, (s7_int)((w->mask[e] & COMPONENT_SCRIPT)
					     ? w->script_ref[e] : 0u)));
}

/* (id . path) rows for every catalog asset of the given ASSET_TYPE_*. */
static s7_pointer assets_of_type(s7_scheme *sc, int type)
{
	s7_pointer out = s7_nil(sc);
	uint32_t   k, n;

	if (!g_asset_api)
		return out;
	n = g_asset_api->count();
	for (k = 0; k < n; k++) {
		struct asset_info mi;

		if (g_asset_api->info(k, &mi) != 0 || mi.type != type ||
		    mi.id == 0)
			continue;
		out = s7_cons(sc,
			      s7_cons(sc, s7_make_integer(sc, (s7_int)mi.id),
				      s7_make_string(sc, mi.path)),
			      out);
	}
	return s7_reverse(sc, out);
}

/* (krudd-mesh-assets) -> ((id . path) ...) for the mesh-binding combo. */
static s7_pointer sp_krudd_mesh_assets(s7_scheme *sc, s7_pointer args)
{
	(void)args;
	return assets_of_type(sc, ASSET_TYPE_MESH);
}

/* (krudd-material-assets) -> ((id . path) ...) for the material-binding combo. */
static s7_pointer sp_krudd_material_assets(s7_scheme *sc, s7_pointer args)
{
	(void)args;
	return assets_of_type(sc, ASSET_TYPE_MATERIAL);
}

/* (krudd-shader-assets) -> ((id . path) ...) for the material's shader combo. */
static s7_pointer sp_krudd_shader_assets(s7_scheme *sc, s7_pointer args)
{
	(void)args;
	return assets_of_type(sc, ASSET_TYPE_SHADER);
}

/* (krudd-script-assets) -> ((id . path) ...) for the script-binding combo. */
static s7_pointer sp_krudd_script_assets(s7_scheme *sc, s7_pointer args)
{
	(void)args;
	return assets_of_type(sc, ASSET_TYPE_SCRIPT);
}

/* (krudd-texture-assets) -> ((id . path) ...) for a material's texture combo. */
static s7_pointer sp_krudd_texture_assets(s7_scheme *sc, s7_pointer args)
{
	(void)args;
	return assets_of_type(sc, ASSET_TYPE_TEXTURE);
}

/* (krudd-asset-find ref) -> the asset's path, or #f when ref is 0 / unknown. */
static s7_pointer sp_krudd_asset_find(s7_scheme *sc, s7_pointer args)
{
	uint32_t          ref = (uint32_t)s7_integer(s7_car(args));
	struct asset_info bi;

	if (ref == 0 || !g_asset_api || !g_asset_api->find ||
	    g_asset_api->find(ref, &bi) != 0)
		return s7_f(sc);
	return s7_make_string(sc, bi.path);
}

/*
 * (krudd-entity-create) -> the new entity id, or -1 on failure. Appends a root
 * entity with an identity transform at the origin, a box mesh, and the
 * built-in checker material — what the "+ Entity" button seeds; the caller
 * names and selects it. Undo is recorded by the scene api.
 */
static s7_pointer sp_krudd_entity_create(s7_scheme *sc, s7_pointer args)
{
	struct transform seed;
	int32_t          id = -1;

	(void)args;
	if (g_entity_api && g_entity_api->create_entity) {
		uint32_t box = asset_id_by_path("builtin://mesh/box");
		uint32_t material =
			asset_id_by_path("builtin://material/checker");

		memset(&seed, 0, sizeof(seed));
		seed.rotation[3] = 1.0f;
		seed.scale[0] = seed.scale[1] = seed.scale[2] = 1.0f;
		id = g_entity_api->create_entity(WORLD_NO_PARENT, &seed, 0u,
						 box);
		if (id >= 0 && material && g_entity_api->set_material_ref)
			g_entity_api->set_material_ref(id, material);
	}
	return s7_make_integer(sc, (s7_int)id);
}

/* (krudd-entity-destroy id) -> unspecified. Tombstones the entity and subtree. */
static s7_pointer sp_krudd_entity_destroy(s7_scheme *sc, s7_pointer args)
{
	int32_t id = (int32_t)s7_integer(s7_car(args));

	if (g_entity_api && g_entity_api->destroy_entity)
		g_entity_api->destroy_entity(id);
	return s7_unspecified(sc);
}

/* (krudd-entity-select id) -> unspecified. Uses g_entity_sel without the api. */
static s7_pointer sp_krudd_entity_select(s7_scheme *sc, s7_pointer args)
{
	int32_t id = (int32_t)s7_integer(s7_car(args));

	if (g_entity_api)
		g_entity_api->set_selected(id);
	else
		g_entity_sel = id;
	return s7_unspecified(sc);
}

/* (krudd-entity-set-name id str) -> unspecified. Empty str clears the name. */
static s7_pointer sp_krudd_entity_set_name(s7_scheme *sc, s7_pointer args)
{
	int32_t    id = (int32_t)s7_integer(s7_car(args));
	s7_pointer nm = s7_cadr(args);

	if (g_entity_api && g_entity_api->set_name && s7_is_string(nm))
		g_entity_api->set_name(id, s7_string(nm));
	return s7_unspecified(sc);
}

/* (krudd-entity-set-transform id (px py pz) (rx ry rz rw) (sx sy sz)). */
static s7_pointer sp_krudd_entity_set_transform(s7_scheme *sc, s7_pointer args)
{
	s7_pointer       p  = args;
	int32_t          id = (int32_t)s7_integer(s7_car(p)); p = s7_cdr(p);
	struct transform nt;

	read_reals(sc, s7_car(p), nt.position, 3); p = s7_cdr(p);
	read_reals(sc, s7_car(p), nt.rotation, 4); p = s7_cdr(p);
	read_reals(sc, s7_car(p), nt.scale, 3);
	if (g_entity_api && g_entity_api->set_transform)
		g_entity_api->set_transform(id, &nt);
	return s7_unspecified(sc);
}

/* (krudd-entity-set-render-ref id ref) -> unspecified. ref 0 unbinds the mesh. */
static s7_pointer sp_krudd_entity_set_render_ref(s7_scheme *sc, s7_pointer args)
{
	int32_t  id  = (int32_t)s7_integer(s7_car(args));
	uint32_t ref = (uint32_t)s7_integer(s7_cadr(args));

	if (g_entity_api && g_entity_api->set_render_ref)
		g_entity_api->set_render_ref(id, ref);
	return s7_unspecified(sc);
}

/* (krudd-entity-set-material-ref id ref) -> unspecified. ref 0 unbinds it. */
static s7_pointer sp_krudd_entity_set_material_ref(s7_scheme *sc, s7_pointer args)
{
	int32_t  id  = (int32_t)s7_integer(s7_car(args));
	uint32_t ref = (uint32_t)s7_integer(s7_cadr(args));

	if (g_entity_api && g_entity_api->set_material_ref)
		g_entity_api->set_material_ref(id, ref);
	return s7_unspecified(sc);
}

/* (krudd-entity-set-script-ref id ref) -> unspecified. ref 0 unbinds it. */
static s7_pointer sp_krudd_entity_set_script_ref(s7_scheme *sc, s7_pointer args)
{
	int32_t  id  = (int32_t)s7_integer(s7_car(args));
	uint32_t ref = (uint32_t)s7_integer(s7_cadr(args));

	if (g_entity_api && g_entity_api->set_script_ref)
		g_entity_api->set_script_ref(id, ref);
	return s7_unspecified(sc);
}

/* (krudd-gizmo-mode) -> the shared gizmo tool: 0 move, 1 rotate, 2 scale. */
static s7_pointer sp_krudd_gizmo_mode(s7_scheme *sc, s7_pointer args)
{
	(void)args;
	return s7_make_integer(sc, (s7_int)g_gizmo_mode);
}

/* (krudd-set-gizmo-mode m) -> unspecified. Out-of-range m is ignored. */
static s7_pointer sp_krudd_set_gizmo_mode(s7_scheme *sc, s7_pointer args)
{
	int m = (int)s7_integer(s7_car(args));

	if (m >= GIZMO_MOVE && m <= GIZMO_SCALE)
		g_gizmo_mode = (enum gizmo_mode)m;
	return s7_unspecified(sc);
}

/*
 * Editor-toolbar accessors (#492) — the undo/redo history and the play/pause
 * simulation toggle that were the ImGui board header's buttons, now driven by
 * kruddgui's own top toolbar (kruddgui.scm). Each mirrors the null-guard the
 * ImGui draw_undo_redo / draw_sim_mode did: with no edit history service the
 * undo/redo controls report empty and no-op, and with no pausing support the
 * sim control reports #f so the toolbar hides it.
 */

/* (krudd-can-undo) -> #t when there is an edit to undo, else #f. */
static s7_pointer sp_krudd_can_undo(s7_scheme *sc, s7_pointer args)
{
	(void)args;
	return s7_make_boolean(sc, g_edit_api && g_edit_api->can_undo());
}

/* (krudd-can-redo) -> #t when there is an undone edit to redo, else #f. */
static s7_pointer sp_krudd_can_redo(s7_scheme *sc, s7_pointer args)
{
	(void)args;
	return s7_make_boolean(sc, g_edit_api && g_edit_api->can_redo());
}

/* (krudd-undo) undo the last edit; a no-op with no history service. */
static s7_pointer sp_krudd_undo(s7_scheme *sc, s7_pointer args)
{
	(void)args;
	if (g_edit_api)
		g_edit_api->undo();
	return s7_unspecified(sc);
}

/* (krudd-redo) redo the last undone edit; a no-op with no history service. */
static s7_pointer sp_krudd_redo(s7_scheme *sc, s7_pointer args)
{
	(void)args;
	if (g_edit_api)
		g_edit_api->redo();
	return s7_unspecified(sc);
}

/*
 * (krudd-sim-mode) -> 'playing or 'paused, or #f when the scene api does not
 * support pausing (the toolbar then omits the play/pause control, exactly as
 * draw_sim_mode drew nothing).
 */
static s7_pointer sp_krudd_sim_mode(s7_scheme *sc, s7_pointer args)
{
	(void)args;
	if (!g_entity_api || !g_entity_api->get_paused ||
	    !g_entity_api->set_paused)
		return s7_make_boolean(sc, false);
	return s7_make_symbol(sc,
			      g_entity_api->get_paused() ? "paused" : "playing");
}

/* (krudd-toggle-sim) flip play/pause; a no-op when pausing is unsupported. */
static s7_pointer sp_krudd_toggle_sim(s7_scheme *sc, s7_pointer args)
{
	(void)args;
	if (g_entity_api && g_entity_api->get_paused && g_entity_api->set_paused)
		g_entity_api->set_paused(!g_entity_api->get_paused());
	return s7_unspecified(sc);
}

/* ------------------------------------------------------------------ */
/* Assets-tab primitives (#402)                                        */
/* ------------------------------------------------------------------ */

/* Buffer cap for the multiline text-edit and asset-data primitives below;
 * matches the pre-port g_edit static buffer size. */
#define ASSETS_EDIT_MAX (64 * 1024)

/*
 * Shader authoring metadata.  A krudd shader is a single DSL source that
 * embeds every stage it defines (see shader.scm) — there is no per-asset
 * stage or dialect to pick, so the editor has nothing to ask for beyond a
 * name; the source itself is the only thing to author.
 */
#define SHADER_FORMAT "krudd-shader"

/*
 * Script authoring metadata.  A krudd script is a single (script NAME ...) form
 * carrying its lifecycle hooks (see core/entity_script.scm) — like a shader,
 * the source itself is the only thing to author, so the editor asks only for a
 * name and derives everything else from the text.
 */
#define SCRIPT_FORMAT "krudd-script"

/*
 * Mesh authoring metadata. A krudd mesh is a single
 * (mesh NAME (generate () ...)) form carrying its generator (see
 * core/mesh_script.scm) — there is no other kind of mesh asset, so the
 * source itself is the only thing to author, the same as a script.
 */
#define MESH_FORMAT "krudd-mesh"

/*
 * Report the stage blocks SRC defines, the same way the built-in shaders
 * advertise theirs via describe() — a comma-joined "vertex, fragment" list
 * in declaration order.  Used both to display the (derived, read-only)
 * declaration and to publish it back on Save.
 */
static const char *shader_stages_from_source(const char *src)
{
	static char buf[32];
	int         has_vertex   = src && strstr(src, "(vertex")   != NULL;
	int         has_fragment = src && strstr(src, "(fragment") != NULL;

	if (has_vertex && has_fragment)
		snprintf(buf, sizeof(buf), "vertex, fragment");
	else if (has_vertex)
		snprintf(buf, sizeof(buf), "vertex");
	else if (has_fragment)
		snprintf(buf, sizeof(buf), "fragment");
	else
		buf[0] = '\0';
	return buf;
}

/*
 * Try to transpile every stage SRC declares. A declared stage that fails to
 * transpile — or no declared stage at all — means the shader can't bind, so
 * treat it as a compile failure rather than committing broken source.
 */
static bool shader_compiles(const char *src)
{
	int has_vertex   = src && strstr(src, "(vertex")   != NULL;
	int has_fragment = src && strstr(src, "(fragment") != NULL;

	if (!has_vertex && !has_fragment)
		return false;
	if (has_vertex && !script_shader_transpile(src, "vertex"))
		return false;
	if (has_fragment && !script_shader_transpile(src, "fragment"))
		return false;
	return true;
}

/*
 * The engine's built-in scene-textured shader — always present, read-only —
 * used to seed new shader assets so they start from working source instead
 * of blank.
 */
static const char *default_shader_src(void)
{
	uint32_t          n, i;
	struct asset_info info;

	if (!g_asset_api)
		return "";
	n = g_asset_api->count();
	for (i = 0; i < n; i++) {
		if (g_asset_api->info(i, &info) != 0 || !info.read_only ||
		    info.type != ASSET_TYPE_SHADER)
			continue;
		if (strcmp(info.path, "builtin://shader/scene-textured") == 0) {
			const void *data = g_asset_api->get_data(info.id, NULL);
			return data ? (const char *)data : "";
		}
	}
	return "";
}

/*
 * Report the lifecycle hooks SRC defines, the same comma-joined form the
 * built-in scripts advertise via describe() ("on-begin, on-tick"), in the
 * canonical begin/tick/destroy order.  Used both to display the (derived,
 * read-only) declaration and to publish it back on Save.  The buffer holds the
 * longest possible list ("on-begin, on-tick, on-destroy") with room to spare.
 */
static const char *script_hooks_from_source(const char *src)
{
	static char buf[64];
	int         has_begin   = src && strstr(src, "(on-begin")   != NULL;
	int         has_tick    = src && strstr(src, "(on-tick")    != NULL;
	int         has_destroy = src && strstr(src, "(on-destroy") != NULL;
	int         n           = 0;

	buf[0] = '\0';
	if (has_begin) {
		strcpy(buf, "on-begin");
		n++;
	}
	if (has_tick) {
		if (n)
			strcat(buf, ", ");
		strcat(buf, "on-tick");
		n++;
	}
	if (has_destroy) {
		if (n)
			strcat(buf, ", ");
		strcat(buf, "on-destroy");
	}
	return buf;
}

/*
 * A script is well-formed enough to commit when it is a (script ...) form that
 * declares at least one lifecycle hook — the script analogue of the shader
 * save's compile gate, kept to a cheap substring test (matching shader_stages_
 * from_source's style) so a Save never has to eval untrusted source.  Blank or
 * hookless text is rejected, so a broken edit never reaches the live asset.
 */
static bool script_form_ok(const char *src)
{
	if (!src || !strstr(src, "(script"))
		return false;
	return strstr(src, "(on-begin") != NULL
	    || strstr(src, "(on-tick") != NULL
	    || strstr(src, "(on-destroy") != NULL;
}

/*
 * The engine's built-in spinner script — always present, read-only — used to
 * seed new script assets so they start from a working (script ...) form
 * instead of blank, the way default_shader_src seeds new shaders.
 */
static const char *default_script_src(void)
{
	uint32_t          n, i;
	struct asset_info info;

	if (!g_asset_api)
		return "";
	n = g_asset_api->count();
	for (i = 0; i < n; i++) {
		if (g_asset_api->info(i, &info) != 0 || !info.read_only ||
		    info.type != ASSET_TYPE_SCRIPT)
			continue;
		if (strcmp(info.path, "builtin://script/spinner") == 0) {
			const void *data = g_asset_api->get_data(info.id, NULL);
			return data ? (const char *)data : "";
		}
	}
	return "";
}

/*
 * A mesh is well-formed enough to commit when it is a (mesh ...) form
 * declaring a (generate ...) clause — the mesh analogue of script_form_ok's
 * cheap substring gate, so a Save never has to eval untrusted source. Blank
 * or generate-less text is rejected, so a broken edit never reaches the live
 * asset.
 */
static bool mesh_form_ok(const char *src)
{
	if (!src || !strstr(src, "(mesh"))
		return false;
	return strstr(src, "(generate") != NULL;
}

/*
 * The engine's built-in grid mesh — always present, read-only — used to seed
 * new mesh assets so they start from working source instead of blank, the
 * way default_script_src seeds new entity scripts.
 */
static const char *default_mesh_src(void)
{
	uint32_t          n, i;
	struct asset_info info;

	if (!g_asset_api)
		return "";
	n = g_asset_api->count();
	for (i = 0; i < n; i++) {
		if (g_asset_api->info(i, &info) != 0 || !info.read_only ||
		    info.type != ASSET_TYPE_MESH)
			continue;
		if (strcmp(info.path, "builtin://mesh/grid") == 0) {
			const void *data = g_asset_api->get_data(info.id, NULL);
			return data ? (const char *)data : "";
		}
	}
	return "";
}

/*
 * Persist an authored asset's current bytes through the backend, if the
 * backend is present and capable — the same "write it live even without
 * persistence" fallback every Assets save button used pre-port (#390).
 * Looks the path up fresh via find() so every mutation primitive below can
 * share this instead of threading an asset_info through each call.
 */
static void maybe_persist_asset(uint32_t id, int32_t type, const void *bytes,
				uint32_t size)
{
	struct asset_info info;

	if (!g_backend || !(g_backend->get_caps() & BACKEND_CAP_PROJECT_PERSIST))
		return;
	if (!g_asset_api || g_asset_api->find(id, &info) != 0)
		return;
	g_backend->persist_asset(id, info.path, type, bytes, size);
}

/* One (id path type kind state size refs) row for the asset browser. */
static s7_pointer asset_row(s7_scheme *sc, const struct asset_info *info)
{
	return s7_list(sc, 7,
		s7_make_integer(sc, (s7_int)info->id),
		s7_make_string(sc, info->path),
		s7_make_integer(sc, info->type),
		s7_make_integer(sc, info->kind),
		s7_make_integer(sc, info->state),
		s7_make_integer(sc, (s7_int)info->size),
		s7_make_integer(sc, info->refs));
}

/*
 * (krudd-assets) -> (builtin-rows project-rows), each a list of (id path
 * type kind state size refs) rows in catalog order, or #f when the asset
 * api is absent. Split by read_only here so the browser table's two labeled
 * groups need no filtering in Scheme.
 */
static s7_pointer sp_krudd_assets(s7_scheme *sc, s7_pointer args)
{
	s7_pointer builtin = s7_nil(sc);
	s7_pointer project = s7_nil(sc);
	uint32_t   n, i;

	(void)args;
	if (!g_asset_api)
		return s7_f(sc);
	n = g_asset_api->count();
	for (i = 0; i < n; i++) {
		struct asset_info info;

		if (g_asset_api->info(i, &info) != 0)
			continue;
		if (info.read_only)
			builtin = s7_cons(sc, asset_row(sc, &info), builtin);
		else
			project = s7_cons(sc, asset_row(sc, &info), project);
	}
	return s7_list(sc, 2, s7_reverse(sc, builtin), s7_reverse(sc, project));
}

/* (krudd-asset-mut?) -> #t when the mutation api is present (editor build). */
static s7_pointer sp_krudd_asset_mut(s7_scheme *sc, s7_pointer args)
{
	(void)args;
	return s7_make_boolean(sc, g_asset_mut != NULL);
}

/*
 * (krudd-asset-info id) -> (path type kind state size refs read-only?
 * origin), or #f when id is unknown.
 */
static s7_pointer sp_krudd_asset_info(s7_scheme *sc, s7_pointer args)
{
	uint32_t          id = (uint32_t)s7_integer(s7_car(args));
	struct asset_info info;

	if (!g_asset_api || g_asset_api->find(id, &info) != 0)
		return s7_f(sc);
	return s7_list(sc, 8,
		s7_make_string(sc, info.path),
		s7_make_integer(sc, info.type),
		s7_make_integer(sc, info.kind),
		s7_make_integer(sc, info.state),
		s7_make_integer(sc, (s7_int)info.size),
		s7_make_integer(sc, info.refs),
		s7_make_boolean(sc, info.read_only),
		s7_make_integer(sc, info.origin));
}

/*
 * (krudd-asset-describe id) -> ((key . value) ...), the declaration fields
 * describe() reports for id, or '() when id is unknown or has none. describe
 * is index-addressed, so this scans the catalog once to find id's index.
 */
static s7_pointer sp_krudd_asset_describe(s7_scheme *sc, s7_pointer args)
{
	uint32_t                 id = (uint32_t)s7_integer(s7_car(args));
	struct asset_decl_field  fields[16];
	struct asset_info        tmp;
	s7_pointer               out = s7_nil(sc);
	uint32_t                 n, i, idx, nf;

	if (!g_asset_api)
		return out;
	n   = g_asset_api->count();
	idx = n;
	for (i = 0; i < n; i++) {
		if (g_asset_api->info(i, &tmp) == 0 && tmp.id == id) {
			idx = i;
			break;
		}
	}
	if (idx >= n)
		return out;
	nf = g_asset_api->describe(idx, fields, 16);
	for (i = nf; i > 0; i--)
		out = s7_cons(sc,
			s7_cons(sc, s7_make_string(sc, fields[i - 1].key),
				s7_make_string(sc, fields[i - 1].value)),
			out);
	return out;
}

/*
 * (krudd-asset-data id) -> the asset's bytes as a string, clamped to
 * ASSETS_EDIT_MAX - 1 and NUL-terminated; "" when id is unknown or has no
 * data. Used to (re)load the text/shader source edit buffer on selection.
 */
static s7_pointer sp_krudd_asset_data(s7_scheme *sc, s7_pointer args)
{
	static char buf[ASSETS_EDIT_MAX];
	uint32_t    id = (uint32_t)s7_integer(s7_car(args));
	const void *src;
	uint32_t    sz = 0;

	buf[0] = '\0';
	if (g_asset_api && g_asset_api->get_data &&
	    (src = g_asset_api->get_data(id, &sz)) != NULL) {
		if (sz >= (uint32_t)ASSETS_EDIT_MAX)
			sz = (uint32_t)ASSETS_EDIT_MAX - 1;
		memcpy(buf, src, (size_t)sz);
		buf[sz] = '\0';
	}
	return s7_make_string(sc, buf);
}

/*
 * A material's wire form is a leading uint32 shader-ref (asset id — a material
 * always names its shader) followed by the shader's std140 Material block. The
 * schema (which params, their layout) lives entirely in the shader; these
 * helpers introspect it (via the runtime image's shader-material-params) and
 * pack/unpack a material's values against it.
 */
#define MATERIAL_MAX_PARAMS 32
#define MATERIAL_HEADER_BYTES ((uint32_t)sizeof(uint32_t))
#define MATERIAL_WIRE_CAP (MATERIAL_HEADER_BYTES + 256u)

/*
 * Borrow a shader asset's source as a NUL-terminated C string in a static
 * buffer. Authored shader bytes are stored without a trailing NUL (set_data
 * takes strlen()), so copy-and-terminate rather than handing get_data() straight
 * to the parser. Single static buffer — each caller consumes it before the next.
 */
static const char *shader_src_cstr(uint32_t shader_ref)
{
	static char buf[8192];
	const void *d;
	uint32_t    sz = 0;

	if (!g_asset_api || !g_asset_api->get_data)
		return NULL;
	d = g_asset_api->get_data(shader_ref, &sz);
	if (!d)
		return NULL;
	if (sz >= sizeof(buf))
		sz = sizeof(buf) - 1;
	memcpy(buf, d, sz);
	buf[sz] = '\0';
	return buf;
}

/* The default value of editable component k: an authored (default V ...) if the
 * field declares one, else the value implied by its edit hint. */
static float param_default(const struct shader_param *p, uint32_t k)
{
	if (k < p->default_count)
		return p->edit_default[k];
	if (strcmp(p->edit, "color") == 0)
		return 1.0f;                 /* opaque white / full */
	if (strcmp(p->edit, "range") == 0)
		return p->edit_min;
	return 0.0f;
}

/* Resolve a catalog path to its stable asset id, or 0 when absent. */
static uint32_t asset_id_by_path(const char *path)
{
	uint32_t i, n;

	if (!g_asset_api)
		return 0;
	n = g_asset_api->count();
	for (i = 0; i < n; i++) {
		struct asset_info info;

		if (g_asset_api->info(i, &info) == 0 &&
		    strcmp(info.path, path) == 0)
			return info.id;
	}
	return 0;
}

/*
 * Pack a material's wire bytes: the shader-ref header then each field's floats
 * at its std140 offset, per the shader's layout. Every field is written — from
 * field_values (a Scheme list of per-field component lists) where supplied, else
 * from its edit-hint default — so a material created with no values still lands
 * on sensible defaults (white for a color) rather than zeros. Returns the total
 * byte length written into out (capped at cap).
 */
static uint32_t pack_material(s7_scheme *sc, uint32_t shader_ref,
			      const char *src, s7_pointer field_values,
			      unsigned char *out, uint32_t cap)
{
	struct shader_param p[MATERIAL_MAX_PARAMS];
	uint32_t            total = 0, len;
	int                 n, i;
	s7_pointer          fv = field_values;

	n = script_shader_material_params(src, p, MATERIAL_MAX_PARAMS, &total);
	if (n < 0)
		n = 0;
	len = MATERIAL_HEADER_BYTES + total;
	if (len > cap)
		len = cap;
	memset(out, 0, len);
	memcpy(out, &shader_ref, sizeof(shader_ref));

	for (i = 0; i < n; i++) {
		uint32_t c   = p[i].components > 4 ? 4 : p[i].components;
		uint32_t off = MATERIAL_HEADER_BYTES + p[i].offset;
		float    v[4];
		uint32_t k;

		for (k = 0; k < c; k++)
			v[k] = param_default(&p[i], k);
		if (s7_is_pair(fv)) {
			read_reals(sc, s7_car(fv), v, (int)c);
			fv = s7_cdr(fv);
		}
		if (off + c * sizeof(float) <= len)
			memcpy(out + off, v, c * sizeof(float));
	}
	return len;
}

/* The std140 byte size of a shader's Material block (0 when it has none). */
static uint32_t mat_block_len(uint32_t shader_ref)
{
	const char *src   = shader_src_cstr(shader_ref);
	uint32_t    total = 0;

	if (src)
		script_shader_material_params(src, NULL, 0, &total);
	return total;
}

/*
 * Locate a material's optional texture slot — the trailer after the shader-ref
 * and the shader's Material block: [tex-ref u32][width u32][height u32]. Returns
 * 1 and fills the outs when present, 0 otherwise. The block size comes from the
 * material's own shader (its leading ref), the same split the renderer makes, so
 * a material with no trailer reports no texture.
 */
static int mat_texture_slot(uint32_t id, uint32_t *tex, uint32_t *w, uint32_t *h)
{
	const uint8_t *bytes;
	uint32_t       sz = 0, shader_ref = 0, block, off;

	if (!g_asset_api || !g_asset_api->get_data)
		return 0;
	bytes = (const uint8_t *)g_asset_api->get_data(id, &sz);
	if (!bytes || sz < MATERIAL_HEADER_BYTES)
		return 0;
	memcpy(&shader_ref, bytes, sizeof(shader_ref));
	block = mat_block_len(shader_ref);
	if (block == 0)
		return 0;
	off = MATERIAL_HEADER_BYTES + block;
	if (sz < off + 3u * sizeof(uint32_t))
		return 0;
	memcpy(tex, bytes + off,      sizeof(uint32_t));
	memcpy(w,   bytes + off + 4u, sizeof(uint32_t));
	memcpy(h,   bytes + off + 8u, sizeof(uint32_t));
	return *tex != 0;
}

/*
 * (krudd-material-texture id) -> (tex-ref width height) when the material binds a
 * texture, else '(). The material editor reads this to seed its texture picker
 * and resolution control.
 */
static s7_pointer sp_krudd_material_texture(s7_scheme *sc, s7_pointer args)
{
	uint32_t id = (uint32_t)s7_integer(s7_car(args));
	uint32_t tex = 0, w = 0, h = 0;

	if (!mat_texture_slot(id, &tex, &w, &h))
		return s7_nil(sc);
	return s7_list(sc, 3, s7_make_integer(sc, (s7_int)tex),
		       s7_make_integer(sc, (s7_int)w),
		       s7_make_integer(sc, (s7_int)h));
}

/*
 * (krudd-shader-material-params shader-ref) -> ((name type off size components
 * edit-kind edit-min edit-max) ...), the shader's Material block as editable
 * parameters. Empty when the shader is gone or declares no Material block.
 */
static s7_pointer sp_krudd_shader_material_params(s7_scheme *sc, s7_pointer args)
{
	uint32_t            shader_ref = (uint32_t)s7_integer(s7_car(args));
	const char         *src        = shader_src_cstr(shader_ref);
	struct shader_param p[MATERIAL_MAX_PARAMS];
	uint32_t            total = 0;
	int                 n, i;
	s7_pointer          out = s7_nil(sc);

	if (!src)
		return out;
	n = script_shader_material_params(src, p, MATERIAL_MAX_PARAMS, &total);
	for (i = n - 1; i >= 0; i--)
		out = s7_cons(sc,
			s7_list(sc, 8,
				s7_make_string(sc, p[i].name),
				s7_make_string(sc, p[i].type),
				s7_make_integer(sc, (s7_int)p[i].offset),
				s7_make_integer(sc, (s7_int)p[i].size),
				s7_make_integer(sc, (s7_int)p[i].components),
				s7_make_string(sc, p[i].edit),
				s7_make_real(sc, p[i].edit_min),
				s7_make_real(sc, p[i].edit_max)),
			out);
	return out;
}

/*
 * (krudd-asset-shader-ref id) -> the material's shader asset id (its leading
 * uint32), or 0 when it has none. The editor reads this to seed the shader
 * picker with the material's current shader.
 */
static s7_pointer sp_krudd_asset_shader_ref(s7_scheme *sc, s7_pointer args)
{
	uint32_t     id  = (uint32_t)s7_integer(s7_car(args));
	uint32_t     ref = 0;
	const void  *src;
	uint32_t     sz  = 0;

	if (g_asset_api && g_asset_api->get_data &&
	    (src = g_asset_api->get_data(id, &sz)) != NULL &&
	    sz >= MATERIAL_HEADER_BYTES)
		memcpy(&ref, src, sizeof(ref));
	return s7_make_integer(sc, (s7_int)ref);
}

/*
 * (krudd-material-values material-id shader-ref) -> a per-field list of
 * component lists ((v0 v1 ..) ...), the material's current parameter values
 * laid out for shader-ref. Values are read from the material's packed bytes only
 * when it already targets this shader (its leading shader-ref matches); switching
 * to a different shader yields that shader's per-hint defaults instead.
 */
static s7_pointer sp_krudd_material_values(s7_scheme *sc, s7_pointer args)
{
	uint32_t             mat_id     = (uint32_t)s7_integer(s7_car(args));
	uint32_t             shader_ref = (uint32_t)s7_integer(s7_cadr(args));
	const char          *src        = shader_src_cstr(shader_ref);
	struct shader_param  p[MATERIAL_MAX_PARAMS];
	uint32_t             total = 0, msz = 0, stored = 0;
	const unsigned char *mb = NULL;
	int                  n, i, use_stored = 0;
	s7_pointer           out = s7_nil(sc);

	if (!src)
		return out;
	n = script_shader_material_params(src, p, MATERIAL_MAX_PARAMS, &total);

	if (g_asset_api && g_asset_api->get_data)
		mb = (const unsigned char *)g_asset_api->get_data(mat_id, &msz);
	if (mb && msz >= MATERIAL_HEADER_BYTES) {
		memcpy(&stored, mb, sizeof(stored));
		use_stored = (stored == shader_ref);
	}

	for (i = n - 1; i >= 0; i--) {
		uint32_t c   = p[i].components > 4 ? 4 : p[i].components;
		uint32_t off = MATERIAL_HEADER_BYTES + p[i].offset;
		float    v[4];
		uint32_t k;

		for (k = 0; k < c; k++)
			v[k] = param_default(&p[i], k);
		if (use_stored && off + c * sizeof(float) <= msz)
			memcpy(v, mb + off, c * sizeof(float));
		out = s7_cons(sc, real_list(sc, v, (int)c), out);
	}
	return out;
}

/*
 * Entity script parameters — the CPU-side twin of the shader/material helpers
 * above. A script's params clause is its schema (introspected tight-packed, no
 * std140 header); an entity's override blob holds the values. These three feed
 * the same widget dispatcher the material editor uses, keyed on the entity.
 */

/*
 * (krudd-script-params script-ref) -> ((name type off size components edit-kind
 * edit-min edit-max) ...), the bound script's params clause as editable
 * parameters. Empty when the asset is gone or declares no params.
 */
static s7_pointer sp_krudd_script_params(s7_scheme *sc, s7_pointer args)
{
	uint32_t            script_ref = (uint32_t)s7_integer(s7_car(args));
	const char         *src        = shader_src_cstr(script_ref);
	struct shader_param p[MATERIAL_MAX_PARAMS];
	uint32_t            total = 0;
	int                 n, i;
	s7_pointer          out = s7_nil(sc);

	if (!src)
		return out;
	n = script_entity_params(src, p, MATERIAL_MAX_PARAMS, &total);
	for (i = n - 1; i >= 0; i--)
		out = s7_cons(sc,
			s7_list(sc, 8,
				s7_make_string(sc, p[i].name),
				s7_make_string(sc, p[i].type),
				s7_make_integer(sc, (s7_int)p[i].offset),
				s7_make_integer(sc, (s7_int)p[i].size),
				s7_make_integer(sc, (s7_int)p[i].components),
				s7_make_string(sc, p[i].edit),
				s7_make_real(sc, p[i].edit_min),
				s7_make_real(sc, p[i].edit_max)),
			out);
	return out;
}

/*
 * (krudd-entity-script-values entity-id script-ref) -> a per-field list of
 * component lists, entity-id's current values for script-ref's params: read from
 * its override blob where present, else the param's edit-hint default. This is
 * the override ⊕ defaults the entity menu edits (and the script sees).
 */
static s7_pointer sp_krudd_entity_script_values(s7_scheme *sc, s7_pointer args)
{
	int32_t              eid        = (int32_t)s7_integer(s7_car(args));
	uint32_t             script_ref = (uint32_t)s7_integer(s7_cadr(args));
	const char          *src        = shader_src_cstr(script_ref);
	struct shader_param  p[MATERIAL_MAX_PARAMS];
	uint32_t             total = 0, blen = 0;
	const uint8_t       *blob = NULL;
	const struct world  *w    = NULL;
	int                  n, i;
	s7_pointer           out = s7_nil(sc);

	if (!src)
		return out;
	n = script_entity_params(src, p, MATERIAL_MAX_PARAMS, &total);
	if (g_entity_api && g_entity_api->get_world)
		w = g_entity_api->get_world();
	if (w && eid >= 0 && (uint32_t)eid < w->count)
		blob = world_script_params(w, (uint32_t)eid, &blen);

	for (i = n - 1; i >= 0; i--) {
		uint32_t c   = p[i].components > 4 ? 4 : p[i].components;
		float    v[4];
		uint32_t k;

		for (k = 0; k < c; k++)
			v[k] = param_default(&p[i], k);
		if (blob && p[i].offset + c * sizeof(float) <= blen)
			memcpy(v, blob + p[i].offset, c * sizeof(float));
		out = s7_cons(sc, real_list(sc, v, (int)c), out);
	}
	return out;
}

/*
 * (krudd-entity-save-script-params entity-id script-ref field-values) ->
 * unspecified. Packs the per-field values into script-ref's tight params layout
 * and stores them as entity-id's override (through the scene api, so it records
 * an undo step). field-values is a list of per-field component lists.
 */
static s7_pointer sp_krudd_entity_save_script_params(s7_scheme *sc,
						     s7_pointer args)
{
	s7_pointer          a          = args;
	int32_t             eid        = (int32_t)s7_integer(s7_car(a));
	uint32_t            script_ref;
	s7_pointer          values;
	const char         *src;
	struct shader_param p[MATERIAL_MAX_PARAMS];
	uint8_t             bytes[WORLD_SCRIPT_PARAM_CAP];
	uint32_t            total = 0, len;
	int                 n, i;
	s7_pointer          fv;

	a          = s7_cdr(a);
	script_ref = (uint32_t)s7_integer(s7_car(a));
	a          = s7_cdr(a);
	values     = s7_car(a);
	fv         = values;
	src        = shader_src_cstr(script_ref);
	if (!src)
		return s7_unspecified(sc);

	n = script_entity_params(src, p, MATERIAL_MAX_PARAMS, &total);
	if (n < 0)
		n = 0;
	len = total > sizeof(bytes) ? (uint32_t)sizeof(bytes) : total;
	memset(bytes, 0, len);

	for (i = 0; i < n; i++) {
		uint32_t c   = p[i].components > 4 ? 4 : p[i].components;
		uint32_t off = p[i].offset;
		float    v[4];
		uint32_t k;

		for (k = 0; k < c; k++)
			v[k] = param_default(&p[i], k);
		if (s7_is_pair(fv)) {
			read_reals(sc, s7_car(fv), v, (int)c);
			fv = s7_cdr(fv);
		}
		if (off + c * sizeof(float) <= len)
			memcpy(bytes + off, v, c * sizeof(float));
	}
	if (g_entity_api && g_entity_api->set_script_params)
		g_entity_api->set_script_params(eid, bytes, len);
	return s7_unspecified(sc);
}

/*
 * (krudd-entity-material-values entity-id material-id shader-ref) -> a per-field
 * list of component lists, entity-id's current values for the material's params:
 * its per-entity override where present, else the shared material asset's stored
 * values, else the shader's per-hint defaults. This is the override ⊕ material ⊕
 * defaults the entity inspector edits (and the renderer draws with). Mirrors
 * krudd-material-values, but overlaid with the entity's override so the swatch
 * shows what this one entity draws, not what the shared material stores.
 */
static s7_pointer sp_krudd_entity_material_values(s7_scheme *sc, s7_pointer args)
{
	s7_pointer           a          = args;
	int32_t              eid        = (int32_t)s7_integer(s7_car(a));
	uint32_t             mat_id;
	uint32_t             shader_ref;
	const char          *src;
	struct shader_param  p[MATERIAL_MAX_PARAMS];
	uint32_t             total = 0, msz = 0, stored = 0, blen = 0;
	const unsigned char *mb   = NULL;
	const uint8_t       *blob = NULL;
	const struct world  *w    = NULL;
	int                  n, i, use_stored = 0;
	s7_pointer           out = s7_nil(sc);

	a          = s7_cdr(a);
	mat_id     = (uint32_t)s7_integer(s7_car(a));
	a          = s7_cdr(a);
	shader_ref = (uint32_t)s7_integer(s7_car(a));
	src        = shader_src_cstr(shader_ref);
	if (!src)
		return out;
	n = script_shader_material_params(src, p, MATERIAL_MAX_PARAMS, &total);

	if (g_asset_api && g_asset_api->get_data)
		mb = (const unsigned char *)g_asset_api->get_data(mat_id, &msz);
	if (mb && msz >= MATERIAL_HEADER_BYTES) {
		memcpy(&stored, mb, sizeof(stored));
		use_stored = (stored == shader_ref);
	}
	if (g_entity_api && g_entity_api->get_world)
		w = g_entity_api->get_world();
	if (w && eid >= 0 && (uint32_t)eid < w->count)
		blob = world_material_params(w, (uint32_t)eid, &blen);

	for (i = n - 1; i >= 0; i--) {
		uint32_t c   = p[i].components > 4 ? 4 : p[i].components;
		uint32_t off = MATERIAL_HEADER_BYTES + p[i].offset;
		float    v[4];
		uint32_t k;

		for (k = 0; k < c; k++)
			v[k] = param_default(&p[i], k);
		/* Shared material's stored value, then the entity override on top
		 * (offsets: the material carries the shader-ref header, the
		 * override is header-less std140 like the UBO the renderer binds). */
		if (use_stored && off + c * sizeof(float) <= msz)
			memcpy(v, mb + off, c * sizeof(float));
		if (blob && p[i].offset + c * sizeof(float) <= blen)
			memcpy(v, blob + p[i].offset, c * sizeof(float));
		out = s7_cons(sc, real_list(sc, v, (int)c), out);
	}
	return out;
}

/*
 * (krudd-entity-save-material-params entity-id shader-ref field-values) ->
 * unspecified. Packs the per-field values into shader-ref's std140 Material
 * layout — header-less, exactly the bytes the renderer uploads to the Material
 * UBO — and stores them as entity-id's per-entity override (through the scene
 * api, so it records an undo step). The mirror of krudd-entity-save-script-params
 * for materials; field-values is a list of per-field component lists.
 */
static s7_pointer sp_krudd_entity_save_material_params(s7_scheme *sc,
						       s7_pointer args)
{
	s7_pointer          a          = args;
	int32_t             eid        = (int32_t)s7_integer(s7_car(a));
	uint32_t            shader_ref;
	s7_pointer          values;
	const char         *src;
	struct shader_param p[MATERIAL_MAX_PARAMS];
	uint8_t             bytes[WORLD_MATERIAL_PARAM_CAP];
	uint32_t            total = 0, len;
	int                 n, i;
	s7_pointer          fv;

	a          = s7_cdr(a);
	shader_ref = (uint32_t)s7_integer(s7_car(a));
	a          = s7_cdr(a);
	values     = s7_car(a);
	fv         = values;
	src        = shader_src_cstr(shader_ref);
	if (!src)
		return s7_unspecified(sc);

	n = script_shader_material_params(src, p, MATERIAL_MAX_PARAMS, &total);
	if (n < 0)
		n = 0;
	len = total > sizeof(bytes) ? (uint32_t)sizeof(bytes) : total;
	memset(bytes, 0, len);

	for (i = 0; i < n; i++) {
		uint32_t c   = p[i].components > 4 ? 4 : p[i].components;
		uint32_t off = p[i].offset;
		float    v[4];
		uint32_t k;

		for (k = 0; k < c; k++)
			v[k] = param_default(&p[i], k);
		if (s7_is_pair(fv)) {
			read_reals(sc, s7_car(fv), v, (int)c);
			fv = s7_cdr(fv);
		}
		if (off + c * sizeof(float) <= len)
			memcpy(bytes + off, v, c * sizeof(float));
	}
	if (g_entity_api && g_entity_api->set_material_params)
		g_entity_api->set_material_params(eid, bytes, len);
	return s7_unspecified(sc);
}

/*
 * Mesh parameters — the geometry twin of the script helpers above. A mesh's
 * params clause is its schema (introspected tight-packed, like a script's, no
 * std140 header); an entity's override blob holds the values. These three feed
 * the same widget dispatcher the script and material editors use, keyed on the
 * entity, so editing a box's width is the same gesture as editing a tint.
 */

/*
 * (krudd-mesh-params mesh-ref) -> ((name type off size components edit-kind
 * edit-min edit-max) ...), the bound mesh's params clause as editable
 * parameters. Empty when the asset is gone or declares no params.
 */
static s7_pointer sp_krudd_mesh_params(s7_scheme *sc, s7_pointer args)
{
	uint32_t            mesh_ref = (uint32_t)s7_integer(s7_car(args));
	const char         *src      = shader_src_cstr(mesh_ref);
	struct shader_param p[MATERIAL_MAX_PARAMS];
	uint32_t            total = 0;
	int                 n, i;
	s7_pointer          out = s7_nil(sc);

	if (!src)
		return out;
	n = script_mesh_params(src, p, MATERIAL_MAX_PARAMS, &total);
	for (i = n - 1; i >= 0; i--)
		out = s7_cons(sc,
			s7_list(sc, 8,
				s7_make_string(sc, p[i].name),
				s7_make_string(sc, p[i].type),
				s7_make_integer(sc, (s7_int)p[i].offset),
				s7_make_integer(sc, (s7_int)p[i].size),
				s7_make_integer(sc, (s7_int)p[i].components),
				s7_make_string(sc, p[i].edit),
				s7_make_real(sc, p[i].edit_min),
				s7_make_real(sc, p[i].edit_max)),
			out);
	return out;
}

/*
 * (krudd-entity-mesh-values entity-id mesh-ref) -> a per-field list of component
 * lists, entity-id's current values for mesh-ref's params: read from its override
 * blob where present, else the param's edit-hint default. This is the override ⊕
 * defaults the entity inspector edits (and the generator sees). The mesh mirror
 * of krudd-entity-script-values.
 */
static s7_pointer sp_krudd_entity_mesh_values(s7_scheme *sc, s7_pointer args)
{
	int32_t              eid      = (int32_t)s7_integer(s7_car(args));
	uint32_t             mesh_ref = (uint32_t)s7_integer(s7_cadr(args));
	const char          *src      = shader_src_cstr(mesh_ref);
	struct shader_param  p[MATERIAL_MAX_PARAMS];
	uint32_t             total = 0, blen = 0;
	const uint8_t       *blob = NULL;
	const struct world  *w    = NULL;
	int                  n, i;
	s7_pointer           out = s7_nil(sc);

	if (!src)
		return out;
	n = script_mesh_params(src, p, MATERIAL_MAX_PARAMS, &total);
	if (g_entity_api && g_entity_api->get_world)
		w = g_entity_api->get_world();
	if (w && eid >= 0 && (uint32_t)eid < w->count)
		blob = world_mesh_params(w, (uint32_t)eid, &blen);

	for (i = n - 1; i >= 0; i--) {
		uint32_t c   = p[i].components > 4 ? 4 : p[i].components;
		float    v[4];
		uint32_t k;

		for (k = 0; k < c; k++)
			v[k] = param_default(&p[i], k);
		if (blob && p[i].offset + c * sizeof(float) <= blen)
			memcpy(v, blob + p[i].offset, c * sizeof(float));
		out = s7_cons(sc, real_list(sc, v, (int)c), out);
	}
	return out;
}

/*
 * (krudd-entity-save-mesh-params entity-id mesh-ref field-values) -> unspecified.
 * Packs the per-field values into mesh-ref's tight params layout and stores them
 * as entity-id's override (through the scene api, so it records an undo step).
 * The mirror of krudd-entity-save-script-params for meshes; field-values is a
 * list of per-field component lists.
 */
static s7_pointer sp_krudd_entity_save_mesh_params(s7_scheme *sc,
						   s7_pointer args)
{
	s7_pointer          a        = args;
	int32_t             eid      = (int32_t)s7_integer(s7_car(a));
	uint32_t            mesh_ref;
	s7_pointer          values;
	const char         *src;
	struct shader_param p[MATERIAL_MAX_PARAMS];
	uint8_t             bytes[WORLD_MESH_PARAM_CAP];
	uint32_t            total = 0, len;
	int                 n, i;
	s7_pointer          fv;

	a        = s7_cdr(a);
	mesh_ref = (uint32_t)s7_integer(s7_car(a));
	a        = s7_cdr(a);
	values   = s7_car(a);
	fv       = values;
	src      = shader_src_cstr(mesh_ref);
	if (!src)
		return s7_unspecified(sc);

	n = script_mesh_params(src, p, MATERIAL_MAX_PARAMS, &total);
	if (n < 0)
		n = 0;
	len = total > sizeof(bytes) ? (uint32_t)sizeof(bytes) : total;
	memset(bytes, 0, len);

	for (i = 0; i < n; i++) {
		uint32_t c   = p[i].components > 4 ? 4 : p[i].components;
		uint32_t off = p[i].offset;
		float    v[4];
		uint32_t k;

		for (k = 0; k < c; k++)
			v[k] = param_default(&p[i], k);
		if (s7_is_pair(fv)) {
			read_reals(sc, s7_car(fv), v, (int)c);
			fv = s7_cdr(fv);
		}
		if (off + c * sizeof(float) <= len)
			memcpy(bytes + off, v, c * sizeof(float));
	}
	if (g_entity_api && g_entity_api->set_mesh_params)
		g_entity_api->set_mesh_params(eid, bytes, len);
	return s7_unspecified(sc);
}

/*
 * (krudd-texture-params texture-ref) -> ((name type off size components edit-kind
 * edit-min edit-max) ...), the texture's params clause as editable parameters —
 * the pixel mirror of krudd-mesh-params. Empty when the asset is gone or declares
 * no params.
 */
static s7_pointer sp_krudd_texture_params(s7_scheme *sc, s7_pointer args)
{
	uint32_t            tex_ref = (uint32_t)s7_integer(s7_car(args));
	const char         *src     = shader_src_cstr(tex_ref);
	struct shader_param p[MATERIAL_MAX_PARAMS];
	uint32_t            total = 0;
	int                 n, i;
	s7_pointer          out = s7_nil(sc);

	if (!src)
		return out;
	n = script_texture_params(src, p, MATERIAL_MAX_PARAMS, &total);
	for (i = n - 1; i >= 0; i--)
		out = s7_cons(sc,
			s7_list(sc, 8,
				s7_make_string(sc, p[i].name),
				s7_make_string(sc, p[i].type),
				s7_make_integer(sc, (s7_int)p[i].offset),
				s7_make_integer(sc, (s7_int)p[i].size),
				s7_make_integer(sc, (s7_int)p[i].components),
				s7_make_string(sc, p[i].edit),
				s7_make_real(sc, p[i].edit_min),
				s7_make_real(sc, p[i].edit_max)),
			out);
	return out;
}

/*
 * (krudd-entity-texture-values entity-id texture-ref) -> a per-field list of
 * component lists, entity-id's current values for texture-ref's params: read from
 * its override blob where present, else the param's edit-hint default — the
 * override ⊕ defaults the entity inspector edits and the baker sees. The texture
 * mirror of krudd-entity-mesh-values.
 */
static s7_pointer sp_krudd_entity_texture_values(s7_scheme *sc, s7_pointer args)
{
	int32_t              eid     = (int32_t)s7_integer(s7_car(args));
	uint32_t             tex_ref = (uint32_t)s7_integer(s7_cadr(args));
	const char          *src     = shader_src_cstr(tex_ref);
	struct shader_param  p[MATERIAL_MAX_PARAMS];
	uint32_t             total = 0, blen = 0;
	const uint8_t       *blob = NULL;
	const struct world  *w    = NULL;
	int                  n, i;
	s7_pointer           out = s7_nil(sc);

	if (!src)
		return out;
	n = script_texture_params(src, p, MATERIAL_MAX_PARAMS, &total);
	if (g_entity_api && g_entity_api->get_world)
		w = g_entity_api->get_world();
	if (w && eid >= 0 && (uint32_t)eid < w->count)
		blob = world_texture_params(w, (uint32_t)eid, &blen);

	for (i = n - 1; i >= 0; i--) {
		uint32_t c   = p[i].components > 4 ? 4 : p[i].components;
		float    v[4];
		uint32_t k;

		for (k = 0; k < c; k++)
			v[k] = param_default(&p[i], k);
		if (blob && p[i].offset + c * sizeof(float) <= blen)
			memcpy(v, blob + p[i].offset, c * sizeof(float));
		out = s7_cons(sc, real_list(sc, v, (int)c), out);
	}
	return out;
}

/*
 * (krudd-entity-save-texture-params entity-id texture-ref field-values) ->
 * unspecified. Packs the per-field values into texture-ref's tight params layout
 * and stores them as entity-id's override (through the scene api, so it records
 * an undo step). The texture mirror of krudd-entity-save-mesh-params.
 */
static s7_pointer sp_krudd_entity_save_texture_params(s7_scheme *sc,
						      s7_pointer args)
{
	s7_pointer          a       = args;
	int32_t             eid     = (int32_t)s7_integer(s7_car(a));
	uint32_t            tex_ref;
	s7_pointer          values;
	const char         *src;
	struct shader_param p[MATERIAL_MAX_PARAMS];
	uint8_t             bytes[WORLD_TEXTURE_PARAM_CAP];
	uint32_t            total = 0, len;
	int                 n, i;
	s7_pointer          fv;

	a       = s7_cdr(a);
	tex_ref = (uint32_t)s7_integer(s7_car(a));
	a       = s7_cdr(a);
	values  = s7_car(a);
	fv      = values;
	src     = shader_src_cstr(tex_ref);
	if (!src)
		return s7_unspecified(sc);

	n = script_texture_params(src, p, MATERIAL_MAX_PARAMS, &total);
	if (n < 0)
		n = 0;
	len = total > sizeof(bytes) ? (uint32_t)sizeof(bytes) : total;
	memset(bytes, 0, len);

	for (i = 0; i < n; i++) {
		uint32_t c   = p[i].components > 4 ? 4 : p[i].components;
		uint32_t off = p[i].offset;
		float    v[4];
		uint32_t k;

		for (k = 0; k < c; k++)
			v[k] = param_default(&p[i], k);
		if (s7_is_pair(fv)) {
			read_reals(sc, s7_car(fv), v, (int)c);
			fv = s7_cdr(fv);
		}
		if (off + c * sizeof(float) <= len)
			memcpy(bytes + off, v, c * sizeof(float));
	}
	if (g_entity_api && g_entity_api->set_texture_params)
		g_entity_api->set_texture_params(eid, bytes, len);
	return s7_unspecified(sc);
}

/*
 * (krudd-texture-values texture-ref) -> a per-field list of component lists, the
 * texture's declared parameter defaults (its params clause, no override) — the
 * seed the Assets-tab texture inspector's live-preview sliders start from, the
 * asset-level twin of krudd-entity-texture-values (which overlays an entity's
 * override). Empty when the asset is gone or declares no params.
 */
static s7_pointer sp_krudd_texture_values(s7_scheme *sc, s7_pointer args)
{
	uint32_t             tex_ref = (uint32_t)s7_integer(s7_car(args));
	const char          *src     = shader_src_cstr(tex_ref);
	struct shader_param  p[MATERIAL_MAX_PARAMS];
	uint32_t             total = 0;
	int                  n, i;
	s7_pointer           out = s7_nil(sc);

	if (!src)
		return out;
	n = script_texture_params(src, p, MATERIAL_MAX_PARAMS, &total);
	for (i = n - 1; i >= 0; i--) {
		uint32_t c = p[i].components > 4 ? 4 : p[i].components;
		float    v[4];
		uint32_t k;

		for (k = 0; k < c; k++)
			v[k] = param_default(&p[i], k);
		out = s7_cons(sc, real_list(sc, v, (int)c), out);
	}
	return out;
}

/*
 * Live texture-preview cache. One device texture is baked from the inspected
 * texture's (source, params, resolution) and re-baked only when that key
 * changes, so a still frame costs nothing and dragging a slider re-bakes. The
 * bake rides the renderer's gpu_api (texture_create/destroy) rather than raw
 * GL, so this file stays backend-agnostic ahead of the WebGPU move; kruddgui
 * blits the texture's native handle with kgui-image. texture_create uploads
 * level 0 once, so a re-bake drops the old texture and creates a fresh one.
 */
static gpu_texture_t g_tex_prev_tex; /* NULL = not yet allocated            */
static uint32_t g_tex_prev_ref;   /* asset id of the last successful bake  */
static uint32_t g_tex_prev_res;   /* edge length of the last bake          */
static uint32_t g_tex_prev_hash;  /* FNV-1a of the last packed params      */
static int      g_tex_prev_valid; /* did the last bake succeed?            */

static uint32_t tex_prev_hash(const uint8_t *b, uint32_t n)
{
	uint32_t h = 2166136261u, i;

	for (i = 0; i < n; i++) {
		h ^= b[i];
		h *= 16777619u;
	}
	return h;
}

/*
 * bake_texture_preview(sc, tex_ref, values, res) -> the backend-native texture
 * handle (the GL name on WebGL) holding a live res x res bake of the texture, or
 * 0 when it can't be baked (source gone, renderer/memory api absent, or the
 * shade clause faults). field-values is the per-field list the Parameters
 * sliders edit; it is packed into the texture's tight params layout (the same
 * packing the entity texture-param save does, minus the world write) and fed to
 * texture_script_generate, so a slider drag re-bakes. res is clamped to a
 * preview-sized edge. The cached texture is re-baked only when (ref, res,
 * packed-params) changes, so a still frame costs nothing. sp_krudd_texture_bake
 * hands the resulting handle to kruddgui's kgui-image.
 */
static uint32_t bake_texture_preview(s7_scheme *sc, uint32_t tex_ref,
				     s7_pointer values, uint32_t res)
{
	const char           *src = shader_src_cstr(tex_ref);
	const struct gpu_api *gpu = g_mgr ?
		(const struct gpu_api *)
			subsystem_manager_get_api(g_mgr, "renderer") : NULL;
	struct shader_param   p[MATERIAL_MAX_PARAMS];
	uint8_t               bytes[WORLD_TEXTURE_PARAM_CAP];
	uint32_t              total = 0, len, hash;
	int                   n, i;
	s7_pointer            fv = values;

	if (!src || res == 0 || !gpu)
		return 0;
	if (res > 256)
		res = 256;

	n = script_texture_params(src, p, MATERIAL_MAX_PARAMS, &total);
	if (n < 0)
		n = 0;
	len = total > sizeof(bytes) ? (uint32_t)sizeof(bytes) : total;
	memset(bytes, 0, len);
	for (i = 0; i < n; i++) {
		uint32_t c   = p[i].components > 4 ? 4 : p[i].components;
		uint32_t off = p[i].offset;
		float    v[4];
		uint32_t k;

		for (k = 0; k < c; k++)
			v[k] = param_default(&p[i], k);
		if (s7_is_pair(fv)) {
			read_reals(sc, s7_car(fv), v, (int)c);
			fv = s7_cdr(fv);
		}
		if (off + c * sizeof(float) <= len)
			memcpy(bytes + off, v, c * sizeof(float));
	}
	hash = tex_prev_hash(bytes, len);

	if (!(g_tex_prev_tex && g_tex_prev_valid && g_tex_prev_ref == tex_ref &&
	      g_tex_prev_res == res && g_tex_prev_hash == hash)) {
		struct texture_blob *b =
			g_mem ? texture_script_generate(src, bytes, len, res, res,
							g_mem, NULL)
			      : NULL;

		g_tex_prev_ref   = tex_ref;
		g_tex_prev_res   = res;
		g_tex_prev_hash  = hash;
		g_tex_prev_valid = (b != NULL);
		if (b) {
			struct gpu_texture_desc td;

			/*
			 * texture_create uploads level 0 at create time and never
			 * re-uploads, so a re-bake drops the previous texture and
			 * makes a fresh one. The backend picks the sampler state
			 * (the same it uses for procedural material textures), so
			 * the preview samples exactly as the scene does.
			 */
			if (g_tex_prev_tex)
				gpu->texture_destroy(g_tex_prev_tex);

			memset(&td, 0, sizeof(td));
			td.format       = GPU_FORMAT_RGBA8_UNORM;
			td.width        = b->width;
			td.height       = b->height;
			td.mip_levels   = 1;
			td.sample_count = 1;
			td.initial_data = texture_blob_pixels(b);

			g_tex_prev_tex   = gpu->texture_create(&td);
			g_tex_prev_valid = (g_tex_prev_tex != NULL);
			g_mem->free(b);
		}
	}

	return (g_tex_prev_valid && g_tex_prev_tex) ?
		gpu->texture_native_handle(g_tex_prev_tex) : 0;
}

/*
 * (krudd-texture-bake texture-ref field-values res) -> the native texture handle
 * of the live bake, or 0 when it can't be baked. kruddgui draws the returned
 * handle itself with kgui-image, so the baked pixels present through its own
 * quad batch.
 */
static s7_pointer sp_krudd_texture_bake(s7_scheme *sc, s7_pointer args)
{
	s7_pointer a       = args;
	uint32_t   tex_ref = (uint32_t)s7_integer(s7_car(a)); a = s7_cdr(a);
	s7_pointer values  = s7_car(a);                      a = s7_cdr(a);
	uint32_t   res     = (uint32_t)s7_integer(s7_car(a));

	return s7_make_integer(sc,
			       (s7_int)bake_texture_preview(sc, tex_ref, values,
							    res));
}

/*
 * (krudd-mesh-bake mesh-ref material-ref res) -> the GL texture id of a shaded
 * offscreen render of the mesh, or 0 when the preview service is absent or the
 * render fails. Runs scene_renderer's preview pass (preview_api.h) into a render-
 * target texture, spinning by a wall-clock-derived yaw, and returns the handle
 * for kruddgui to blit with kgui-image. material-ref 0 uses the built-in default
 * material, so the mesh inspector shows pure lit geometry.
 */
static s7_pointer sp_krudd_mesh_bake(s7_scheme *sc, s7_pointer args)
{
	s7_pointer a        = args;
	uint32_t   mesh_ref = (uint32_t)s7_integer(s7_car(a)); a = s7_cdr(a);
	uint32_t   mat_ref  = (uint32_t)s7_integer(s7_car(a)); a = s7_cdr(a);
	uint32_t   res      = (uint32_t)s7_integer(s7_car(a));
	float      yaw      = (float)(emscripten_get_now() * 0.0005);

	if (!g_preview_api || !g_preview_api->render_mesh || res == 0)
		return s7_make_integer(sc, 0);
	return s7_make_integer(sc,
			       (s7_int)g_preview_api->render_mesh(mesh_ref, mat_ref,
								  res, yaw));
}

/*
 * Override flags — the per-field twin of the *-values accessors above, one
 * boolean per param: is this entity's value an override that differs from the
 * baseline it would draw without one? The World tree lights a dot beside a
 * widget when its flag is #t. For a script or mesh the baseline is the param's
 * declared default; for a material it is the shared asset's stored value (else
 * the shader default), so the dot means "this one entity overrides the shared
 * material", never "the material author set a non-default". A value edited back
 * to its baseline clears the flag, so the dot tracks "customized", not "ever
 * touched". Order matches the *-values list exactly, so map can walk both.
 */

/* True when any of C floats in an override blob differ from base[]; #f (0)
 * when the entity has no override covering them, so an untouched param reads
 * as not-overridden. */
static int blob_differs(const uint8_t *blob, uint32_t off, uint32_t blen,
			const float *base, uint32_t c)
{
	uint32_t k;
	float    ov;

	if (!blob || off + c * sizeof(float) > blen)
		return 0;
	for (k = 0; k < c; k++) {
		memcpy(&ov, blob + off + k * sizeof(float), sizeof(float));
		if (ov != base[k])
			return 1;
	}
	return 0;
}

static s7_pointer sp_krudd_entity_script_overrides(s7_scheme *sc,
						   s7_pointer args)
{
	int32_t              eid = (int32_t)s7_integer(s7_car(args));
	uint32_t             ref = (uint32_t)s7_integer(s7_cadr(args));
	const char          *src = shader_src_cstr(ref);
	struct shader_param  p[MATERIAL_MAX_PARAMS];
	uint32_t             total = 0, blen = 0;
	const uint8_t       *blob = NULL;
	const struct world  *w    = NULL;
	int                  n, i;
	s7_pointer           out = s7_nil(sc);

	if (!src)
		return out;
	n = script_entity_params(src, p, MATERIAL_MAX_PARAMS, &total);
	if (g_entity_api && g_entity_api->get_world)
		w = g_entity_api->get_world();
	if (w && eid >= 0 && (uint32_t)eid < w->count)
		blob = world_script_params(w, (uint32_t)eid, &blen);

	for (i = n - 1; i >= 0; i--) {
		uint32_t c = p[i].components > 4 ? 4 : p[i].components;
		float    base[4];
		uint32_t k;

		for (k = 0; k < c; k++)
			base[k] = param_default(&p[i], k);
		out = s7_cons(sc, s7_make_boolean(sc,
			blob_differs(blob, p[i].offset, blen, base, c)), out);
	}
	return out;
}

static s7_pointer sp_krudd_entity_mesh_overrides(s7_scheme *sc, s7_pointer args)
{
	int32_t              eid = (int32_t)s7_integer(s7_car(args));
	uint32_t             ref = (uint32_t)s7_integer(s7_cadr(args));
	const char          *src = shader_src_cstr(ref);
	struct shader_param  p[MATERIAL_MAX_PARAMS];
	uint32_t             total = 0, blen = 0;
	const uint8_t       *blob = NULL;
	const struct world  *w    = NULL;
	int                  n, i;
	s7_pointer           out = s7_nil(sc);

	if (!src)
		return out;
	n = script_mesh_params(src, p, MATERIAL_MAX_PARAMS, &total);
	if (g_entity_api && g_entity_api->get_world)
		w = g_entity_api->get_world();
	if (w && eid >= 0 && (uint32_t)eid < w->count)
		blob = world_mesh_params(w, (uint32_t)eid, &blen);

	for (i = n - 1; i >= 0; i--) {
		uint32_t c = p[i].components > 4 ? 4 : p[i].components;
		float    base[4];
		uint32_t k;

		for (k = 0; k < c; k++)
			base[k] = param_default(&p[i], k);
		out = s7_cons(sc, s7_make_boolean(sc,
			blob_differs(blob, p[i].offset, blen, base, c)), out);
	}
	return out;
}

static s7_pointer sp_krudd_entity_material_overrides(s7_scheme *sc,
						     s7_pointer args)
{
	s7_pointer           a          = args;
	int32_t              eid        = (int32_t)s7_integer(s7_car(a));
	uint32_t             mat_id, shader_ref;
	const char          *src;
	struct shader_param  p[MATERIAL_MAX_PARAMS];
	uint32_t             total = 0, msz = 0, stored = 0, blen = 0;
	const unsigned char *mb   = NULL;
	const uint8_t       *blob = NULL;
	const struct world  *w    = NULL;
	int                  n, i, use_stored = 0;
	s7_pointer           out = s7_nil(sc);

	a          = s7_cdr(a);
	mat_id     = (uint32_t)s7_integer(s7_car(a));
	a          = s7_cdr(a);
	shader_ref = (uint32_t)s7_integer(s7_car(a));
	src        = shader_src_cstr(shader_ref);
	if (!src)
		return out;
	n = script_shader_material_params(src, p, MATERIAL_MAX_PARAMS, &total);
	if (g_asset_api && g_asset_api->get_data)
		mb = (const unsigned char *)g_asset_api->get_data(mat_id, &msz);
	if (mb && msz >= MATERIAL_HEADER_BYTES) {
		memcpy(&stored, mb, sizeof(stored));
		use_stored = (stored == shader_ref);
	}
	if (g_entity_api && g_entity_api->get_world)
		w = g_entity_api->get_world();
	if (w && eid >= 0 && (uint32_t)eid < w->count)
		blob = world_material_params(w, (uint32_t)eid, &blen);

	for (i = n - 1; i >= 0; i--) {
		uint32_t c    = p[i].components > 4 ? 4 : p[i].components;
		uint32_t moff = MATERIAL_HEADER_BYTES + p[i].offset;
		float    base[4];
		uint32_t k;

		/* Baseline the entity draws WITHOUT its override: the shared
		 * material's stored value, else the shader default. */
		for (k = 0; k < c; k++)
			base[k] = param_default(&p[i], k);
		if (use_stored && moff + c * sizeof(float) <= msz)
			memcpy(base, mb + moff, c * sizeof(float));
		out = s7_cons(sc, s7_make_boolean(sc,
			blob_differs(blob, p[i].offset, blen, base, c)), out);
	}
	return out;
}

/* (krudd-shader-stages src) -> the declared stage list, or "" if none. */
static s7_pointer sp_krudd_shader_stages(s7_scheme *sc, s7_pointer args)
{
	s7_pointer src = s7_car(args);

	return s7_make_string(sc,
		shader_stages_from_source(s7_is_string(src) ? s7_string(src) : ""));
}

/* (krudd-asset-save-text id text) -> unspecified. Writes live and persists. */
static s7_pointer sp_krudd_asset_save_text(s7_scheme *sc, s7_pointer args)
{
	uint32_t    id  = (uint32_t)s7_integer(s7_car(args));
	const char *txt = s7_is_string(s7_cadr(args)) ? s7_string(s7_cadr(args)) : "";
	uint32_t    len = (uint32_t)strlen(txt);

	if (g_asset_mut)
		g_asset_mut->set_data(id, txt, len);
	maybe_persist_asset(id, ASSET_TYPE_TEXT, txt, len);
	return s7_unspecified(sc);
}

/*
 * (krudd-asset-save-shader id text) -> #t on a successful compile+save, #f
 * on a failed compile (nothing is committed, so a broken edit never reaches
 * the live asset). Preserves the shader-transpile validation gate #390 added.
 */
static s7_pointer sp_krudd_asset_save_shader(s7_scheme *sc, s7_pointer args)
{
	uint32_t                 id  = (uint32_t)s7_integer(s7_car(args));
	const char               *txt = s7_is_string(s7_cadr(args))
		? s7_string(s7_cadr(args)) : "";
	uint32_t                 len = (uint32_t)strlen(txt);
	struct asset_decl_field  decl[2];

	if (!shader_compiles(txt))
		return s7_f(sc);

	decl[0].key   = "format";
	decl[0].value = SHADER_FORMAT;
	decl[1].key   = "stages";
	decl[1].value = shader_stages_from_source(txt);

	if (g_asset_mut) {
		g_asset_mut->set_data(id, txt, len);
		if (g_asset_mut->set_decl)
			g_asset_mut->set_decl(id, decl, 2);
	}
	maybe_persist_asset(id, ASSET_TYPE_SHADER, txt, len);
	return s7_t(sc);
}

/*
 * (krudd-asset-save-material id shader-ref field-values) -> unspecified. Packs
 * the per-field values into the shader-ref's std140 Material layout and stores
 * the v3 wire form; field-values is a list of per-field component lists.
 */
static s7_pointer sp_krudd_asset_save_material(s7_scheme *sc, s7_pointer args)
{
	s7_pointer    p          = args;
	uint32_t      id         = (uint32_t)s7_integer(s7_car(p)); p = s7_cdr(p);
	uint32_t      shader_ref = (uint32_t)s7_integer(s7_car(p)); p = s7_cdr(p);
	s7_pointer    values     = s7_car(p);                       p = s7_cdr(p);
	const char   *src        = shader_src_cstr(shader_ref);
	unsigned char bytes[MATERIAL_WIRE_CAP + 3 * sizeof(uint32_t)];
	uint32_t      len;
	uint32_t      tex = 0, tw = 0, th = 0;

	if (!src)
		return s7_unspecified(sc);
	len = pack_material(sc, shader_ref, src, values, bytes, MATERIAL_WIRE_CAP);
	/*
	 * Optional texture slot: trailing (tex-ref width height) appends the
	 * trailer the renderer reads to bake and bind this material's procedural
	 * texture. A tex-ref of 0, absent args, or a zero dimension leaves the
	 * material texture-less — byte-for-byte the pre-texture wire form.
	 */
	if (s7_is_pair(p)) { tex = (uint32_t)s7_integer(s7_car(p)); p = s7_cdr(p); }
	if (s7_is_pair(p)) { tw  = (uint32_t)s7_integer(s7_car(p)); p = s7_cdr(p); }
	if (s7_is_pair(p)) { th  = (uint32_t)s7_integer(s7_car(p)); p = s7_cdr(p); }
	if (tex != 0 && tw > 0 && th > 0 &&
	    len + 3u * sizeof(uint32_t) <= sizeof(bytes)) {
		memcpy(bytes + len, &tex, sizeof(tex)); len += (uint32_t)sizeof(tex);
		memcpy(bytes + len, &tw,  sizeof(tw));  len += (uint32_t)sizeof(tw);
		memcpy(bytes + len, &th,  sizeof(th));  len += (uint32_t)sizeof(th);
	}
	if (g_asset_mut)
		g_asset_mut->set_data(id, bytes, len);
	maybe_persist_asset(id, ASSET_TYPE_MATERIAL, bytes, len);
	return s7_unspecified(sc);
}

/* (krudd-asset-delete id) -> unspecified. Works for any authored type. */
static s7_pointer sp_krudd_asset_delete(s7_scheme *sc, s7_pointer args)
{
	uint32_t id = (uint32_t)s7_integer(s7_car(args));

	if (g_asset_mut)
		g_asset_mut->destroy(id);
	if (g_backend && (g_backend->get_caps() & BACKEND_CAP_PROJECT_PERSIST))
		g_backend->delete_asset(id);
	return s7_unspecified(sc);
}

/* (krudd-asset-create-text path) -> the new id, or 0 on failure. */
static s7_pointer sp_krudd_asset_create_text(s7_scheme *sc, s7_pointer args)
{
	s7_pointer  path = s7_car(args);
	const char *p    = s7_is_string(path) ? s7_string(path) : "";
	uint32_t    nid  = 0;

	if (g_asset_mut)
		nid = g_asset_mut->create(p, ASSET_TYPE_TEXT, "", 0);
	if (nid != 0)
		maybe_persist_asset(nid, ASSET_TYPE_TEXT, "", 0);
	return s7_make_integer(sc, (s7_int)nid);
}

/*
 * (krudd-asset-create-shader path) -> the new id, or 0 on failure. Seeds
 * from the built-in scene-textured shader so authoring starts from working
 * source; no declaration is set yet, matching the pre-port New Asset flow —
 * the first successful Save publishes it.
 */
static s7_pointer sp_krudd_asset_create_shader(s7_scheme *sc, s7_pointer args)
{
	s7_pointer  path = s7_car(args);
	const char *p    = s7_is_string(path) ? s7_string(path) : "";
	const char *seed = default_shader_src();
	uint32_t    len  = (uint32_t)strlen(seed);
	uint32_t    nid  = 0;

	if (g_asset_mut)
		nid = g_asset_mut->create(p, ASSET_TYPE_SHADER, seed, len);
	if (nid != 0)
		maybe_persist_asset(nid, ASSET_TYPE_SHADER, seed, len);
	return s7_make_integer(sc, (s7_int)nid);
}

/*
 * (krudd-asset-create-material path) -> the new id, or 0 on failure. A new
 * material names the built-in scene-textured shader and takes that shader's
 * default parameter values (its base_color defaults to white), packed in the
 * v3 wire form — a material is never shaderless.
 */
static s7_pointer sp_krudd_asset_create_material(s7_scheme *sc, s7_pointer args)
{
	const char   *p          = s7_is_string(s7_car(args))
		? s7_string(s7_car(args)) : "";
	uint32_t      shader_ref = asset_id_by_path("builtin://shader/scene-textured");
	const char   *src        = shader_src_cstr(shader_ref);
	unsigned char bytes[MATERIAL_WIRE_CAP];
	uint32_t      len        = MATERIAL_HEADER_BYTES;
	uint32_t      nid        = 0;

	if (src)
		len = pack_material(sc, shader_ref, src, s7_nil(sc), bytes,
				    sizeof(bytes));
	else
		memcpy(bytes, &shader_ref, sizeof(shader_ref));
	if (g_asset_mut)
		nid = g_asset_mut->create(p, ASSET_TYPE_MATERIAL, bytes, len);
	if (nid != 0)
		maybe_persist_asset(nid, ASSET_TYPE_MATERIAL, bytes, len);
	return s7_make_integer(sc, (s7_int)nid);
}

/*
 * (krudd-asset-clone-shader name text) -> the new id, or 0 on failure (e.g.
 * a duplicate path) — the built-in shader "Clone" flow's whole commit:
 * create, publish the derived declaration, persist.
 */
static s7_pointer sp_krudd_asset_clone_shader(s7_scheme *sc, s7_pointer args)
{
	s7_pointer               name = s7_car(args);
	s7_pointer               text = s7_cadr(args);
	const char              *nm   = s7_is_string(name) ? s7_string(name) : "";
	const char              *txt  = s7_is_string(text) ? s7_string(text) : "";
	uint32_t                 len  = (uint32_t)strlen(txt);
	uint32_t                 nid  = 0;
	struct asset_decl_field  decl[2];

	if (g_asset_mut)
		nid = g_asset_mut->create(nm, ASSET_TYPE_SHADER, txt, len);
	if (nid == 0)
		return s7_make_integer(sc, 0);

	decl[0].key   = "format";
	decl[0].value = SHADER_FORMAT;
	decl[1].key   = "stages";
	decl[1].value = shader_stages_from_source(txt);
	if (g_asset_mut->set_decl)
		g_asset_mut->set_decl(nid, decl, 2);
	maybe_persist_asset(nid, ASSET_TYPE_SHADER, txt, len);
	return s7_make_integer(sc, (s7_int)nid);
}

/* (krudd-script-hooks src) -> the declared hook list, or "" if none. */
static s7_pointer sp_krudd_script_hooks(s7_scheme *sc, s7_pointer args)
{
	s7_pointer src = s7_car(args);

	return s7_make_string(sc,
		script_hooks_from_source(s7_is_string(src) ? s7_string(src) : ""));
}

/*
 * (krudd-asset-save-script id text) -> #t on a well-formed save, #f when the
 * source is not a (script ...) form with at least one hook (nothing is
 * committed, so a broken edit never reaches the live asset) — the script
 * analogue of krudd-asset-save-shader's compile gate.
 */
static s7_pointer sp_krudd_asset_save_script(s7_scheme *sc, s7_pointer args)
{
	uint32_t                 id  = (uint32_t)s7_integer(s7_car(args));
	const char               *txt = s7_is_string(s7_cadr(args))
		? s7_string(s7_cadr(args)) : "";
	uint32_t                 len = (uint32_t)strlen(txt);
	struct asset_decl_field  decl[2];

	if (!script_form_ok(txt))
		return s7_f(sc);

	decl[0].key   = "format";
	decl[0].value = SCRIPT_FORMAT;
	decl[1].key   = "hooks";
	decl[1].value = script_hooks_from_source(txt);

	if (g_asset_mut) {
		g_asset_mut->set_data(id, txt, len);
		if (g_asset_mut->set_decl)
			g_asset_mut->set_decl(id, decl, 2);
	}
	maybe_persist_asset(id, ASSET_TYPE_SCRIPT, txt, len);
	return s7_t(sc);
}

/*
 * (krudd-asset-create-script path) -> the new id, or 0 on failure. Seeds from
 * the built-in spinner script so authoring starts from a working (script ...)
 * form; no declaration is set yet — the first successful Save publishes it,
 * mirroring krudd-asset-create-shader.
 */
static s7_pointer sp_krudd_asset_create_script(s7_scheme *sc, s7_pointer args)
{
	s7_pointer  path = s7_car(args);
	const char *p    = s7_is_string(path) ? s7_string(path) : "";
	const char *seed = default_script_src();
	uint32_t    len  = (uint32_t)strlen(seed);
	uint32_t    nid  = 0;

	if (g_asset_mut)
		nid = g_asset_mut->create(p, ASSET_TYPE_SCRIPT, seed, len);
	if (nid != 0)
		maybe_persist_asset(nid, ASSET_TYPE_SCRIPT, seed, len);
	return s7_make_integer(sc, (s7_int)nid);
}

/*
 * (krudd-asset-clone-script name text) -> the new id, or 0 on failure (e.g. a
 * duplicate path) — the built-in script "Clone" flow's whole commit: create,
 * publish the derived declaration, persist. Mirrors krudd-asset-clone-shader.
 */
static s7_pointer sp_krudd_asset_clone_script(s7_scheme *sc, s7_pointer args)
{
	s7_pointer               name = s7_car(args);
	s7_pointer               text = s7_cadr(args);
	const char              *nm   = s7_is_string(name) ? s7_string(name) : "";
	const char              *txt  = s7_is_string(text) ? s7_string(text) : "";
	uint32_t                 len  = (uint32_t)strlen(txt);
	uint32_t                 nid  = 0;
	struct asset_decl_field  decl[2];

	if (g_asset_mut)
		nid = g_asset_mut->create(nm, ASSET_TYPE_SCRIPT, txt, len);
	if (nid == 0)
		return s7_make_integer(sc, 0);

	decl[0].key   = "format";
	decl[0].value = SCRIPT_FORMAT;
	decl[1].key   = "hooks";
	decl[1].value = script_hooks_from_source(txt);
	if (g_asset_mut->set_decl)
		g_asset_mut->set_decl(nid, decl, 2);
	maybe_persist_asset(nid, ASSET_TYPE_SCRIPT, txt, len);
	return s7_make_integer(sc, (s7_int)nid);
}

/*
 * (krudd-asset-clone-material name shader-ref field-values [tex-ref w h]) ->
 * the new id, or 0 on failure (e.g. a duplicate path) — the built-in material
 * "Clone" flow's whole commit: pack the current shader + values (plus the
 * optional bound texture trailer, mirroring krudd-asset-save-material) into
 * the v3 wire form, create, persist. Mirrors krudd-asset-clone-shader above.
 */
static s7_pointer sp_krudd_asset_clone_material(s7_scheme *sc, s7_pointer args)
{
	s7_pointer    p          = args;
	const char   *nm         = s7_is_string(s7_car(p))
		? s7_string(s7_car(p)) : "";
	uint32_t      shader_ref;
	s7_pointer    values;
	const char   *src;
	unsigned char bytes[MATERIAL_WIRE_CAP + 3 * sizeof(uint32_t)];
	uint32_t      len;
	uint32_t      nid = 0;
	uint32_t      tex = 0, tw = 0, th = 0;

	p          = s7_cdr(p);
	shader_ref = (uint32_t)s7_integer(s7_car(p)); p = s7_cdr(p);
	values     = s7_car(p);                       p = s7_cdr(p);
	src        = shader_src_cstr(shader_ref);
	if (!src)
		return s7_make_integer(sc, 0);

	len = pack_material(sc, shader_ref, src, values, bytes, MATERIAL_WIRE_CAP);
	if (s7_is_pair(p)) { tex = (uint32_t)s7_integer(s7_car(p)); p = s7_cdr(p); }
	if (s7_is_pair(p)) { tw  = (uint32_t)s7_integer(s7_car(p)); p = s7_cdr(p); }
	if (s7_is_pair(p)) { th  = (uint32_t)s7_integer(s7_car(p)); p = s7_cdr(p); }
	if (tex != 0 && tw > 0 && th > 0 &&
	    len + 3u * sizeof(uint32_t) <= sizeof(bytes)) {
		memcpy(bytes + len, &tex, sizeof(tex)); len += (uint32_t)sizeof(tex);
		memcpy(bytes + len, &tw,  sizeof(tw));  len += (uint32_t)sizeof(tw);
		memcpy(bytes + len, &th,  sizeof(th));  len += (uint32_t)sizeof(th);
	}
	if (g_asset_mut)
		nid = g_asset_mut->create(nm, ASSET_TYPE_MATERIAL, bytes, len);
	if (nid != 0)
		maybe_persist_asset(nid, ASSET_TYPE_MATERIAL, bytes, len);
	return s7_make_integer(sc, (s7_int)nid);
}

/*
 * (krudd-asset-save-mesh id text) -> #t on a well-formed save, #f when
 * the source is not a (mesh ...) form with a (generate ...) clause (nothing
 * is committed, so a broken edit never reaches the live asset) — the mesh
 * analogue of krudd-asset-save-script.
 */
static s7_pointer sp_krudd_asset_save_mesh(s7_scheme *sc, s7_pointer args)
{
	uint32_t                 id  = (uint32_t)s7_integer(s7_car(args));
	const char               *txt = s7_is_string(s7_cadr(args))
		? s7_string(s7_cadr(args)) : "";
	uint32_t                 len = (uint32_t)strlen(txt);
	struct asset_decl_field  decl[1];

	if (!mesh_form_ok(txt))
		return s7_f(sc);

	decl[0].key   = "format";
	decl[0].value = MESH_FORMAT;

	if (g_asset_mut) {
		g_asset_mut->set_data(id, txt, len);
		if (g_asset_mut->set_decl)
			g_asset_mut->set_decl(id, decl, 1);
	}
	maybe_persist_asset(id, ASSET_TYPE_MESH, txt, len);
	return s7_t(sc);
}

/*
 * (krudd-asset-create-mesh path) -> the new id, or 0 on failure. Seeds
 * from the built-in grid mesh so authoring starts from a working (mesh ...)
 * form; no declaration is set yet — the first successful Save publishes it,
 * mirroring krudd-asset-create-script.
 */
static s7_pointer sp_krudd_asset_create_mesh(s7_scheme *sc, s7_pointer args)
{
	s7_pointer  path = s7_car(args);
	const char *p    = s7_is_string(path) ? s7_string(path) : "";
	const char *seed = default_mesh_src();
	uint32_t    len  = (uint32_t)strlen(seed);
	uint32_t    nid  = 0;

	if (g_asset_mut)
		nid = g_asset_mut->create(p, ASSET_TYPE_MESH, seed, len);
	if (nid != 0)
		maybe_persist_asset(nid, ASSET_TYPE_MESH, seed, len);
	return s7_make_integer(sc, (s7_int)nid);
}

/*
 * (krudd-asset-clone-mesh name text) -> the new id, or 0 on failure
 * (e.g. a duplicate path) — the built-in mesh "Clone" flow's whole commit:
 * create, publish the derived declaration, persist. Mirrors
 * krudd-asset-clone-script.
 */
static s7_pointer sp_krudd_asset_clone_mesh(s7_scheme *sc, s7_pointer args)
{
	s7_pointer               name = s7_car(args);
	s7_pointer               text = s7_cadr(args);
	const char              *nm   = s7_is_string(name) ? s7_string(name) : "";
	const char              *txt  = s7_is_string(text) ? s7_string(text) : "";
	uint32_t                 len  = (uint32_t)strlen(txt);
	uint32_t                 nid  = 0;
	struct asset_decl_field  decl[1];

	if (g_asset_mut)
		nid = g_asset_mut->create(nm, ASSET_TYPE_MESH, txt, len);
	if (nid == 0)
		return s7_make_integer(sc, 0);

	decl[0].key   = "format";
	decl[0].value = MESH_FORMAT;
	if (g_asset_mut->set_decl)
		g_asset_mut->set_decl(nid, decl, 1);
	maybe_persist_asset(nid, ASSET_TYPE_MESH, txt, len);
	return s7_make_integer(sc, (s7_int)nid);
}

/*
 * A styled run of a block's text — a NUL-terminated slice plus its md style
 * (0 normal, 1 bold, 2 code). md_parse hands back byte-offset spans over the
 * block text; splitting them into ready-to-draw runs here (the byte-accurate
 * memcpy the old md_draw shim did) keeps the Scheme markdown renderer to plain
 * layout, never touching offsets. A block with no spans is one normal run.
 */
static s7_pointer md_block_runs(s7_scheme *sc, const struct md_block *b)
{
	s7_pointer runs = s7_nil(sc);
	uint32_t   len  = (uint32_t)strlen(b->text);
	uint32_t   pos  = 0;
	char       buf[MD_TEXT_MAX];
	int32_t    i;

	/* Append (text . style) for b->text[a, e); clamps to the buffer. */
	#define MD_PUSH_RUN(a, e, style) do {                             \
		uint32_t n_ = (e) - (a);                                  \
		if (n_ >= MD_TEXT_MAX) n_ = MD_TEXT_MAX - 1;              \
		memcpy(buf, b->text + (a), n_); buf[n_] = '\0';          \
		runs = s7_cons(sc, s7_cons(sc, s7_make_string(sc, buf),   \
					   s7_make_integer(sc, (style))), \
			       runs);                                     \
	} while (0)

	if (b->span_count == 0) {
		if (len > 0)
			MD_PUSH_RUN(0u, len, 0);
	} else {
		for (i = 0; i < (int32_t)b->span_count; i++) {
			const struct md_span *sp = &b->spans[i];

			if (sp->start > pos)
				MD_PUSH_RUN(pos, sp->start, 0); /* plain gap */
			MD_PUSH_RUN(sp->start, sp->end,
				    (s7_int)(sp->style & MD_SPAN_CODE ? 2
					     : sp->style & MD_SPAN_BOLD ? 1 : 0));
			pos = sp->end;
		}
		if (pos < len)
			MD_PUSH_RUN(pos, len, 0); /* trailing plain */
	}
	#undef MD_PUSH_RUN
	return s7_reverse(sc, runs);
}

/*
 * (krudd-md-parse text) -> a list of parsed markdown blocks for the kruddgui
 * preview to lay out: each block is (type level (run ...)), type 0 paragraph /
 * 1 heading / 2 list-item / 3 code, run = (text . style). Replaces the old
 * ImGui md-preview: the parse (md_parse) and span splitting stay in C; the
 * layout and drawing move to kruddgui.scm (#492 item 3). Parsed fresh each call,
 * cheap against a frame budget; a non-string argument yields no blocks.
 */
static s7_pointer sp_krudd_md_parse(s7_scheme *sc, s7_pointer args)
{
	static struct md_block blocks[MD_BLOCKS_MAX];
	s7_pointer              text = s7_car(args);
	const char             *src  = s7_is_string(text) ? s7_string(text) : "";
	s7_pointer              out  = s7_nil(sc);
	int32_t                 n    = md_parse(src, blocks, MD_BLOCKS_MAX);
	int32_t                 i;

	for (i = n - 1; i >= 0; i--) {
		const struct md_block *b = &blocks[i];

		out = s7_cons(sc, s7_list(sc, 3,
					  s7_make_integer(sc, b->type),
					  s7_make_integer(sc, b->level),
					  md_block_runs(sc, b)),
			      out);
	}
	return out;
}

} /* extern "C" — s7 callbacks */

/*
 * Start the interpreter (if needed), register the panel primitives, and load
 * the image — once. Idempotent and lazy: it costs nothing until the first
 * Scheme-drawn panel renders. Returns the interpreter, or NULL if it failed
 * to start, in which case callers fall back.
 */
static s7_scheme *ensure_panel_scm(void)
{
	static bool ready;
	s7_scheme  *sc = script_s7();

	if (ready || !sc)
		return sc;
	s7_define_function(sc, "krudd-stats", sp_krudd_stats, 0, 0, false,
			   "(krudd-stats) -> (fps frame-ms frame-count) or #f");
	s7_define_function(sc, "krudd-startup", sp_krudd_startup, 0, 0, false,
			   "(krudd-startup) -> (init-ms first-frame-ms (name . ms) ...) or #f");
	s7_define_function(sc, "krudd-subsystems", sp_krudd_subsystems, 0, 0,
			   false,
			   "(krudd-subsystems) -> rows of (name api? tick? size)");
	s7_define_function(sc, "krudd-log-history", sp_krudd_log_history, 0, 0,
			   false,
			   "(krudd-log-history) -> ((level . text) ...) or #f");
	s7_define_function(sc, "krudd-world-caps", sp_krudd_world_caps, 0, 0,
			   false,
			   "(krudd-world-caps) -> (entity-api? asset-api?)");
	s7_define_function(sc, "krudd-selected", sp_krudd_selected, 0, 0, false,
			   "(krudd-selected) -> selected entity id or -1");
	s7_define_function(sc, "krudd-world-entities", sp_krudd_world_entities,
			   0, 0, false,
			   "(krudd-world-entities) -> ((id . name) ...) or #f");
	s7_define_function(sc, "krudd-entity-inspect", sp_krudd_entity_inspect,
			   1, 0, false,
			   "(krudd-entity-inspect id) -> inspector bundle or #f");
	s7_define_function(sc, "krudd-mesh-assets", sp_krudd_mesh_assets, 0, 0,
			   false, "(krudd-mesh-assets) -> ((id . path) ...)");
	s7_define_function(sc, "krudd-material-assets", sp_krudd_material_assets,
			   0, 0, false,
			   "(krudd-material-assets) -> ((id . path) ...)");
	s7_define_function(sc, "krudd-shader-assets", sp_krudd_shader_assets,
			   0, 0, false,
			   "(krudd-shader-assets) -> ((id . path) ...)");
	s7_define_function(sc, "krudd-script-assets", sp_krudd_script_assets,
			   0, 0, false,
			   "(krudd-script-assets) -> ((id . path) ...)");
	s7_define_function(sc, "krudd-texture-assets", sp_krudd_texture_assets,
			   0, 0, false,
			   "(krudd-texture-assets) -> ((id . path) ...)");
	s7_define_function(sc, "krudd-material-texture", sp_krudd_material_texture,
			   1, 0, false,
			   "(krudd-material-texture id) -> (tex-ref w h) or ()");
	s7_define_function(sc, "krudd-asset-find", sp_krudd_asset_find, 1, 0,
			   false, "(krudd-asset-find ref) -> path or #f");
	s7_define_function(sc, "krudd-entity-create", sp_krudd_entity_create, 0,
			   0, false, "(krudd-entity-create) -> new id or -1");
	s7_define_function(sc, "krudd-entity-destroy", sp_krudd_entity_destroy,
			   1, 0, false, "(krudd-entity-destroy id) tombstone it");
	s7_define_function(sc, "krudd-entity-select", sp_krudd_entity_select, 1,
			   0, false, "(krudd-entity-select id) set the selection");
	s7_define_function(sc, "krudd-entity-set-name", sp_krudd_entity_set_name,
			   2, 0, false,
			   "(krudd-entity-set-name id str) rename the entity");
	s7_define_function(sc, "krudd-entity-set-transform",
			   sp_krudd_entity_set_transform, 4, 0, false,
			   "(krudd-entity-set-transform id pos rot scl)");
	s7_define_function(sc, "krudd-entity-set-render-ref",
			   sp_krudd_entity_set_render_ref, 2, 0, false,
			   "(krudd-entity-set-render-ref id ref) bind a mesh");
	s7_define_function(sc, "krudd-entity-set-material-ref",
			   sp_krudd_entity_set_material_ref, 2, 0, false,
			   "(krudd-entity-set-material-ref id ref) bind material");
	s7_define_function(sc, "krudd-entity-set-script-ref",
			   sp_krudd_entity_set_script_ref, 2, 0, false,
			   "(krudd-entity-set-script-ref id ref) bind script");
	s7_define_function(sc, "krudd-gizmo-mode", sp_krudd_gizmo_mode, 0, 0,
			   false, "(krudd-gizmo-mode) -> 0 move 1 rotate 2 scale");
	s7_define_function(sc, "krudd-set-gizmo-mode", sp_krudd_set_gizmo_mode,
			   1, 0, false, "(krudd-set-gizmo-mode m) set the tool");
	s7_define_function(sc, "krudd-can-undo", sp_krudd_can_undo, 0, 0, false,
			   "(krudd-can-undo) -> #t when an edit can be undone");
	s7_define_function(sc, "krudd-can-redo", sp_krudd_can_redo, 0, 0, false,
			   "(krudd-can-redo) -> #t when an edit can be redone");
	s7_define_function(sc, "krudd-undo", sp_krudd_undo, 0, 0, false,
			   "(krudd-undo) undo the last edit");
	s7_define_function(sc, "krudd-redo", sp_krudd_redo, 0, 0, false,
			   "(krudd-redo) redo the last undone edit");
	s7_define_function(sc, "krudd-sim-mode", sp_krudd_sim_mode, 0, 0, false,
			   "(krudd-sim-mode) -> 'playing / 'paused, or #f");
	s7_define_function(sc, "krudd-toggle-sim", sp_krudd_toggle_sim, 0, 0,
			   false, "(krudd-toggle-sim) flip play/pause");
	s7_define_function(sc, "krudd-assets", sp_krudd_assets, 0, 0, false,
			   "(krudd-assets) -> (builtin-rows project-rows) or #f");
	s7_define_function(sc, "krudd-asset-mut?", sp_krudd_asset_mut, 0, 0,
			   false, "(krudd-asset-mut?) -> #t when mutation is available");
	s7_define_function(sc, "krudd-asset-info", sp_krudd_asset_info, 1, 0,
			   false, "(krudd-asset-info id) -> info tuple or #f");
	s7_define_function(sc, "krudd-asset-describe", sp_krudd_asset_describe,
			   1, 0, false, "(krudd-asset-describe id) -> ((key . value) ...)");
	s7_define_function(sc, "krudd-asset-data", sp_krudd_asset_data, 1, 0,
			   false, "(krudd-asset-data id) -> bytes as a string");
	s7_define_function(sc, "krudd-asset-shader-ref",
			   sp_krudd_asset_shader_ref, 1, 0, false,
			   "(krudd-asset-shader-ref id) -> shader asset id or 0");
	s7_define_function(sc, "krudd-shader-material-params",
			   sp_krudd_shader_material_params, 1, 0, false,
			   "(krudd-shader-material-params shader-ref) -> "
			   "((name type off size comps kind min max) ...)");
	s7_define_function(sc, "krudd-material-values",
			   sp_krudd_material_values, 2, 0, false,
			   "(krudd-material-values mat-id shader-ref) -> "
			   "(component-list ...)");
	s7_define_function(sc, "krudd-script-params",
			   sp_krudd_script_params, 1, 0, false,
			   "(krudd-script-params script-ref) -> "
			   "((name type off size comps kind min max) ...)");
	s7_define_function(sc, "krudd-entity-script-values",
			   sp_krudd_entity_script_values, 2, 0, false,
			   "(krudd-entity-script-values entity-id script-ref) -> "
			   "(component-list ...)");
	s7_define_function(sc, "krudd-entity-save-script-params",
			   sp_krudd_entity_save_script_params, 3, 0, false,
			   "(krudd-entity-save-script-params id script-ref values)");
	s7_define_function(sc, "krudd-entity-material-values",
			   sp_krudd_entity_material_values, 3, 0, false,
			   "(krudd-entity-material-values id mat-id shader-ref) -> "
			   "(component-list ...)");
	s7_define_function(sc, "krudd-entity-save-material-params",
			   sp_krudd_entity_save_material_params, 3, 0, false,
			   "(krudd-entity-save-material-params id shader-ref values)");
	s7_define_function(sc, "krudd-mesh-params",
			   sp_krudd_mesh_params, 1, 0, false,
			   "(krudd-mesh-params mesh-ref) -> "
			   "((name type off size comps kind min max) ...)");
	s7_define_function(sc, "krudd-entity-mesh-values",
			   sp_krudd_entity_mesh_values, 2, 0, false,
			   "(krudd-entity-mesh-values entity-id mesh-ref) -> "
			   "(component-list ...)");
	s7_define_function(sc, "krudd-entity-save-mesh-params",
			   sp_krudd_entity_save_mesh_params, 3, 0, false,
			   "(krudd-entity-save-mesh-params id mesh-ref values)");
	s7_define_function(sc, "krudd-texture-params", sp_krudd_texture_params,
			   1, 0, false,
			   "(krudd-texture-params texture-ref) -> "
			   "((name type off size comps kind min max) ...)");
	s7_define_function(sc, "krudd-entity-texture-values",
			   sp_krudd_entity_texture_values, 2, 0, false,
			   "(krudd-entity-texture-values entity-id texture-ref) -> "
			   "(component-list ...)");
	s7_define_function(sc, "krudd-entity-save-texture-params",
			   sp_krudd_entity_save_texture_params, 3, 0, false,
			   "(krudd-entity-save-texture-params id texture-ref values)");
	s7_define_function(sc, "krudd-texture-values", sp_krudd_texture_values,
			   1, 0, false,
			   "(krudd-texture-values texture-ref) -> "
			   "(component-list ...) of declared defaults");
	s7_define_function(sc, "krudd-texture-bake", sp_krudd_texture_bake,
			   3, 0, false,
			   "(krudd-texture-bake texture-ref values res) -> "
			   "GL texture id of the live bake, or 0");
	s7_define_function(sc, "krudd-mesh-bake", sp_krudd_mesh_bake,
			   3, 0, false,
			   "(krudd-mesh-bake mesh-ref material-ref res) -> "
			   "GL texture id of the shaded render, or 0");
	s7_define_function(sc, "krudd-entity-script-overrides",
			   sp_krudd_entity_script_overrides, 2, 0, false,
			   "(krudd-entity-script-overrides id script-ref) -> "
			   "(overridden? ...)");
	s7_define_function(sc, "krudd-entity-material-overrides",
			   sp_krudd_entity_material_overrides, 3, 0, false,
			   "(krudd-entity-material-overrides id mat shader) -> "
			   "(bool ...)");
	s7_define_function(sc, "krudd-entity-mesh-overrides",
			   sp_krudd_entity_mesh_overrides, 2, 0, false,
			   "(krudd-entity-mesh-overrides id mesh-ref) -> "
			   "(overridden? ...)");
	s7_define_function(sc, "krudd-shader-stages", sp_krudd_shader_stages, 1,
			   0, false, "(krudd-shader-stages src) -> stage list string");
	s7_define_function(sc, "krudd-asset-save-text", sp_krudd_asset_save_text,
			   2, 0, false, "(krudd-asset-save-text id text)");
	s7_define_function(sc, "krudd-asset-save-shader",
			   sp_krudd_asset_save_shader, 2, 0, false,
			   "(krudd-asset-save-shader id text) -> compiled-ok?");
	s7_define_function(sc, "krudd-asset-save-material",
			   sp_krudd_asset_save_material, 3, 3, false,
			   "(krudd-asset-save-material id shader-ref values "
			   "[tex-ref w h])");
	s7_define_function(sc, "krudd-asset-delete", sp_krudd_asset_delete, 1,
			   0, false, "(krudd-asset-delete id)");
	s7_define_function(sc, "krudd-asset-create-text",
			   sp_krudd_asset_create_text, 1, 0, false,
			   "(krudd-asset-create-text path) -> new id or 0");
	s7_define_function(sc, "krudd-asset-create-shader",
			   sp_krudd_asset_create_shader, 1, 0, false,
			   "(krudd-asset-create-shader path) -> new id or 0");
	s7_define_function(sc, "krudd-asset-create-material",
			   sp_krudd_asset_create_material, 1, 0, false,
			   "(krudd-asset-create-material path) -> new id or 0");
	s7_define_function(sc, "krudd-asset-clone-shader",
			   sp_krudd_asset_clone_shader, 2, 0, false,
			   "(krudd-asset-clone-shader name text) -> new id or 0");
	s7_define_function(sc, "krudd-script-hooks", sp_krudd_script_hooks, 1,
			   0, false, "(krudd-script-hooks src) -> hook list string");
	s7_define_function(sc, "krudd-asset-save-script",
			   sp_krudd_asset_save_script, 2, 0, false,
			   "(krudd-asset-save-script id text) -> saved-ok?");
	s7_define_function(sc, "krudd-asset-create-script",
			   sp_krudd_asset_create_script, 1, 0, false,
			   "(krudd-asset-create-script path) -> new id or 0");
	s7_define_function(sc, "krudd-asset-clone-script",
			   sp_krudd_asset_clone_script, 2, 0, false,
			   "(krudd-asset-clone-script name text) -> new id or 0");
	s7_define_function(sc, "krudd-asset-save-mesh",
			   sp_krudd_asset_save_mesh, 2, 0, false,
			   "(krudd-asset-save-mesh id text) -> #t/#f");
	s7_define_function(sc, "krudd-asset-create-mesh",
			   sp_krudd_asset_create_mesh, 1, 0, false,
			   "(krudd-asset-create-mesh path) -> new id or 0");
	s7_define_function(sc, "krudd-asset-clone-mesh",
			   sp_krudd_asset_clone_mesh, 2, 0, false,
			   "(krudd-asset-clone-mesh name text) -> new id or 0");
	s7_define_function(sc, "krudd-asset-clone-material",
			   sp_krudd_asset_clone_material, 3, 3, false,
			   "(krudd-asset-clone-material name shader-ref values "
			   "[tex-ref w h]) -> new id or 0");
	s7_define_function(sc, "krudd-md-parse", sp_krudd_md_parse, 1, 0,
			   false, "(krudd-md-parse text) -> (block ...) for preview");
	ready = true;
	return sc;
}
#endif /* __EMSCRIPTEN__ */

/*
 * All of this board's panels now draw as kruddgui consoles off the accessors
 * above (kruddgui.scm, #491/#492): the KRUDD tab's frame stats / startup profile
 * / subsystem table, the Log console, the World/Scene inspector and the Assets
 * browser and editors. Their ImGui draw paths — draw_tab_krudd / -stats /
 * -subsystems / -world / -assets, the ImGui `imgui-*` Scheme primitives that
 * backed them, and the Assets/kruddboard Scheme images — are gone; g_stats and
 * g_mgr live on because the accessors read them.
 */

/* ------------------------------------------------------------------ */
/* Transform gizmo (#178)                                              */
/* ------------------------------------------------------------------ */

/*
 * Hand-rolled against our own camera matrices rather than pulling in ImGuizmo:
 * the engine already exposes view·projection (#171) and a mutable transform API
 * (#173).  The handles draw on kruddgui's own batch through the viewport overlay
 * seam (kruddgui_api, #492) — flat lines, dots and rings under the editor panels
 * — and read the unclaimed pointer from it, where they drove ImGui's background
 * draw list and io before.  All geometry works in world space and projects
 * through the live camera, so the axes track it for free.
 */

#define GIZMO_AXIS_NONE (-1)

/* A 2D point in CSS pixels — the space the overlay seam draws and points in. */
struct gv2 { float x, y; };

static int32_t          g_gizmo_axis = GIZMO_AXIS_NONE; /* dragging axis, or -1 */
static struct transform g_gizmo_start;                  /* local xform at grab */
static struct gv2       g_gizmo_grab;                   /* pointer pos at grab */
static bool             g_gizmo_gesture;                /* edit begin/commit open */

/* Axis colours, RGBA 0..1: X red, Y green, Z blue. */
static const float GIZMO_AXIS_COL[3][4] = {
	{ 0.90f, 0.27f, 0.27f, 1.0f },
	{ 0.35f, 0.82f, 0.35f, 1.0f },
	{ 0.35f, 0.55f, 0.94f, 1.0f },
};

/*
 * Pick radii in CSS pixels. The tip knob is the finger-first grab target: a
 * 22px radius gives a ~44px touch target (the Apple/Material minimum) so a
 * fingertip lands it without precision. The thin axis shaft stays grabbable at
 * a tighter radius for mouse users who seize it mid-length; the knob pass wins
 * near the tip, which also disambiguates near-parallel axes under a fingertip.
 */
#define GIZMO_KNOB_PICK 22.0f
#define GIZMO_LINE_PICK  8.0f

/*
 * Rotate/scale drag sensitivity, expressed against the shorter viewport
 * dimension so a fixed fraction of the screen means a fixed change regardless
 * of resolution or DPI — unlike the old raw px*0.01 constant, which was
 * pixel-tuned for a mouse on one display. A full-screen drag turns once / adds
 * 4.0 to scale. Move needs no constant: it stays world-accurate via len/axis_px.
 */
#define GIZMO_ROT_FULL   6.2831853f /* radians per full shorter-dim drag */
#define GIZMO_SCALE_FULL 4.0f       /* scale units per full shorter-dim drag */

/*
 * Project a world point through view_proj into overlay pixels. view_proj is
 * column-major (m[col*4+row]); disp is the viewport size.  Returns false when
 * the point is at or behind the camera plane (w <= 0).
 */
static bool gizmo_project(const float vp[16], const float p[3],
			  struct gv2 disp, struct gv2 *out)
{
	float cx = vp[0]*p[0] + vp[4]*p[1] + vp[8]*p[2]  + vp[12];
	float cy = vp[1]*p[0] + vp[5]*p[1] + vp[9]*p[2]  + vp[13];
	float cw = vp[3]*p[0] + vp[7]*p[1] + vp[11]*p[2] + vp[15];

	if (cw <= 1e-4f)
		return false;
	out->x = (cx / cw * 0.5f + 0.5f) * disp.x;
	out->y = (1.0f - (cy / cw * 0.5f + 0.5f)) * disp.y;
	return true;
}

/* Shortest distance from point p to segment ab, in pixels. */
static float gizmo_seg_dist(struct gv2 p, struct gv2 a, struct gv2 b)
{
	float vx = b.x - a.x, vy = b.y - a.y;
	float wx = p.x - a.x, wy = p.y - a.y;
	float len2 = vx*vx + vy*vy;
	float t    = len2 > 1e-6f ? (wx*vx + wy*vy) / len2 : 0.0f;
	float dx, dy;

	if (t < 0.0f) t = 0.0f;
	if (t > 1.0f) t = 1.0f;
	dx = a.x + t*vx - p.x;
	dy = a.y + t*vy - p.y;
	return sqrtf(dx*dx + dy*dy);
}

/* Hamilton product o = a*b (xyzw), then re-normalize into out. */
static void gizmo_quat_mul(float o[4], const float a[4], const float b[4])
{
	float x = a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1];
	float y = a[3]*b[1] - a[0]*b[2] + a[1]*b[3] + a[2]*b[0];
	float z = a[3]*b[2] + a[0]*b[1] - a[1]*b[0] + a[2]*b[3];
	float w = a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2];
	float inv = 1.0f / sqrtf(x*x + y*y + z*z + w*w);

	o[0] = x*inv; o[1] = y*inv; o[2] = z*inv; o[3] = w*inv;
}

/* Push the in-progress local transform back through the mutable API. */
static void gizmo_apply(int32_t e, const struct transform *t)
{
	if (g_entity_api && g_entity_api->set_transform)
		g_entity_api->set_transform(e, t);
}

/*
 * World-space unit length that reads as a constant on-screen handle size:
 * scale the axis by its distance from the eye so far entities don't shrink to
 * nothing and near ones don't fill the view.
 */
static float gizmo_handle_len(const float eye[3], const float origin[3])
{
	float dx = origin[0] - eye[0];
	float dy = origin[1] - eye[1];
	float dz = origin[2] - eye[2];
	float d  = sqrtf(dx*dx + dy*dy + dz*dz);

	return d * 0.18f + 0.05f;
}

/*
 * Draw the selected entity's move/rotate/scale handles over the viewport and
 * process a drag on one of them.  Runs every frame the board is visible; draws
 * nothing (and grabs nothing) when the selection is empty — satisfying "no
 * gizmo when nothing is selected".
 *
 * Returns true when a handle is hot or a drag is in flight, i.e. when the
 * gizmo owns this frame's click, so the click-to-pick pass below stands down
 * instead of reselecting out from under a handle grab.
 */
static bool gizmo_update_and_draw(void)
{
	const struct world *w;
	struct mat4         vp;
	float               eye[3];
	float               origin[3];
	float               len;
	float               dw, dh, px, py;
	struct gv2          disp, ptr;
	int32_t             sel;
	uint32_t            e;
	struct gv2          o2d;
	struct gv2          tip2d[3];
	int32_t             hot = GIZMO_AXIS_NONE;

	if (!g_camera_api || !g_entity_api || !g_kgui)
		return false;

	sel = g_entity_api->get_selected();
	w   = g_entity_api->get_world();
	if (!w || sel < 0 || (uint32_t)sel >= w->count || !w->alive[(uint32_t)sel])
		return false;
	e = (uint32_t)sel;

	/* Anchor at the entity's world origin; write edits to its local xform. */
	origin[0] = w->world_xform[e].position[0];
	origin[1] = w->world_xform[e].position[1];
	origin[2] = w->world_xform[e].position[2];

	g_camera_api->get_view_proj(&vp);
	g_camera_api->get_eye(eye);
	len = gizmo_handle_len(eye, origin);

	g_kgui->viewport(&dw, &dh);
	disp.x = dw; disp.y = dh;
	g_kgui->pointer(&px, &py);
	ptr.x = px; ptr.y = py;

	if (!gizmo_project(vp.m, origin, disp, &o2d))
		return false; /* entity behind the camera */

	/* Project each axis tip; bail an axis that clips behind the camera. */
	bool tip_ok[3];
	for (int a = 0; a < 3; a++) {
		float tip[3] = { origin[0], origin[1], origin[2] };

		tip[a] += len;
		tip_ok[a] = gizmo_project(vp.m, tip, disp, &tip2d[a]);
	}

	/*
	 * Hit-test against the nearest axis line — but only when the pointer is
	 * unobstructed, i.e. not over a kruddgui panel (over_ui), or a drag is
	 * already in flight, so tapping the editor never grabs a handle.
	 */
	if (!g_kgui->over_ui(ptr.x, ptr.y) || g_gizmo_axis != GIZMO_AXIS_NONE) {
		/* Primary pass: the fat tip knobs, finger-first. */
		float best = GIZMO_KNOB_PICK;

		for (int a = 0; a < 3; a++) {
			if (!tip_ok[a])
				continue;
			float dx = ptr.x - tip2d[a].x;
			float dy = ptr.y - tip2d[a].y;
			float d  = sqrtf(dx*dx + dy*dy);
			if (d < best) {
				best = d;
				hot  = a;
			}
		}

		/* Fallback: seize the thin shaft mid-length (mouse-friendly). */
		if (hot == GIZMO_AXIS_NONE) {
			best = GIZMO_LINE_PICK;
			for (int a = 0; a < 3; a++) {
				if (!tip_ok[a])
					continue;
				float d = gizmo_seg_dist(ptr, o2d, tip2d[a]);
				if (d < best) {
					best = d;
					hot  = a;
				}
			}
		}
	}

	/* Grab: begin a single-entry undo gesture and snapshot the start xform. */
	if (g_gizmo_axis == GIZMO_AXIS_NONE && hot != GIZMO_AXIS_NONE &&
	    g_kgui->pointer_clicked()) {
		static const char *const LABEL[3] = {
			"Move Entity", "Rotate Entity", "Scale Entity"
		};

		g_gizmo_axis   = hot;
		g_gizmo_start  = w->local[e];
		g_gizmo_grab   = ptr;
		if (g_edit_api && g_edit_api->begin) {
			g_edit_api->begin(LABEL[g_gizmo_mode]);
			g_gizmo_gesture = true;
		}
	}

	/* Drag: map the pointer motion onto the grabbed axis and write it back. */
	if (g_gizmo_axis != GIZMO_AXIS_NONE && g_kgui->pointer_down()) {
		int             a  = g_gizmo_axis;
		struct transform t = g_gizmo_start;
		float           ax = tip2d[a].x - o2d.x;
		float           ay = tip2d[a].y - o2d.y;
		float           axis_px = sqrtf(ax*ax + ay*ay);
		/* Signed pointer travel along the axis's screen direction. */
		float           mvx = ptr.x - g_gizmo_grab.x;
		float           mvy = ptr.y - g_gizmo_grab.y;
		float           along = axis_px > 1e-3f
					? (mvx*ax + mvy*ay) / axis_px : 0.0f;
		/* Fraction of the shorter viewport dimension travelled. */
		float           span   = dw < dh ? dw : dh;
		float           travel = span > 1e-3f ? along / span : 0.0f;

		if (g_gizmo_mode == GIZMO_MOVE) {
			/* along px * (world units per px along this axis). */
			float world = axis_px > 1e-3f
				      ? along * (len / axis_px) : 0.0f;

			t.position[a] = g_gizmo_start.position[a] + world;
		} else if (g_gizmo_mode == GIZMO_SCALE) {
			float s = g_gizmo_start.scale[a]
				  + travel * GIZMO_SCALE_FULL;

			t.scale[a] = s < 0.01f ? 0.01f : s;
		} else { /* GIZMO_ROTATE */
			float axis[3] = { 0.0f, 0.0f, 0.0f };
			float ang     = travel * GIZMO_ROT_FULL;
			float dq[4];

			axis[a] = 1.0f;
			dq[0] = axis[0] * sinf(ang * 0.5f);
			dq[1] = axis[1] * sinf(ang * 0.5f);
			dq[2] = axis[2] * sinf(ang * 0.5f);
			dq[3] = cosf(ang * 0.5f);
			gizmo_quat_mul(t.rotation, dq, g_gizmo_start.rotation);
		}
		gizmo_apply((int32_t)e, &t);
		hot = a; /* keep the grabbed axis highlighted while dragging */
	}

	/* Release: close the undo gesture so the whole drag is one entry. */
	if (g_gizmo_axis != GIZMO_AXIS_NONE && g_kgui->pointer_released()) {
		if (g_gizmo_gesture && g_edit_api && g_edit_api->commit)
			g_edit_api->commit();
		g_gizmo_gesture = false;
		g_gizmo_axis    = GIZMO_AXIS_NONE;
	}

	/* ---- Render the handles on the overlay batch ---- */
	g_kgui->circle(o2d.x, o2d.y, 4.0f, 0.90f, 0.90f, 0.92f, 1.0f);

	for (int a = 0; a < 3; a++) {
		const float *c  = GIZMO_AXIS_COL[a];
		float        th = (a == hot) ? 4.0f : 2.5f;

		if (!tip_ok[a])
			continue;
		g_kgui->line(o2d.x, o2d.y, tip2d[a].x, tip2d[a].y, th,
			     c[0], c[1], c[2], c[3]);

		/*
		 * Fat knobs so the tip reads as a finger target, not a dot:
		 * sized to sit inside the GIZMO_KNOB_PICK grab radius rather
		 * than the old mouse-precise 5px marks.
		 */
		if (g_gizmo_mode == GIZMO_MOVE) {
			g_kgui->circle(tip2d[a].x, tip2d[a].y,
				       (a == hot) ? 11.0f : 9.0f,
				       c[0], c[1], c[2], c[3]);
		} else if (g_gizmo_mode == GIZMO_SCALE) {
			float r = (a == hot) ? 10.0f : 8.0f;

			g_kgui->rect(tip2d[a].x - r, tip2d[a].y - r, 2.0f * r,
				     2.0f * r, c[0], c[1], c[2], c[3]);
		} else { /* rotate: a ring at the tip */
			g_kgui->ring(tip2d[a].x, tip2d[a].y,
				     (a == hot) ? 12.0f : 10.0f,
				     (a == hot) ? 4.0f : 3.0f,
				     c[0], c[1], c[2], c[3]);
		}
	}

	return g_gizmo_axis != GIZMO_AXIS_NONE || hot != GIZMO_AXIS_NONE;
}

/* ------------------------------------------------------------------ */
/* Click-to-pick: raycast the pointer against entity meshes            */
/* ------------------------------------------------------------------ */

/*
 * Inverse of the gizmo's world->screen path: cast a ray from the clicked pixel
 * and return the live render entity whose mesh it strikes nearest the camera,
 * or -1 when the ray misses everything.  Reuses the same view*projection and
 * DisplaySize the gizmo projects with, so a click lands on exactly the pixels
 * a mesh was drawn to.  Brute force over every triangle of every render entity:
 * the world caps at WORLD_MAX_ENTITIES and the meshes are tiny, so no broad-phase
 * or per-mesh bounds are needed yet.
 *
 * A mesh asset stores (mesh ...) source, not a compiled blob, so each entity's
 * geometry is generated on demand through mesh_script_generate — the same call
 * the renderer uploads with, and crucially with this entity's mesh-param override
 * (world_mesh_params), so a resized box's hit-box matches the box that was drawn
 * rather than the un-parameterized default. Generated per click (not per frame),
 * and the param-less fast path is cached in the image, so the cost is bounded.
 */
static int32_t pick_entity_at(float sx, float sy)
{
	const struct world *w;
	struct mat4         vp;
	float               origin[3];
	float               dir[3];
	float               dw, dh;
	int32_t             best = -1;
	float               best_t = FLT_MAX;
	uint32_t            e;

	if (!g_camera_api || !g_entity_api || !g_asset_api || !g_mem || !g_kgui)
		return -1;
	w = g_entity_api->get_world();
	if (!w)
		return -1;

	g_camera_api->get_view_proj(&vp);
	g_kgui->viewport(&dw, &dh);
	if (ray_from_screen(&vp, sx, sy, dw, dh, origin, dir) != 0)
		return -1;

	for (e = 0; e < w->count; e++) {
		struct mesh_blob         *blob;
		const struct mesh_vertex *vtx;
		const uint16_t           *idx;
		const char               *src;
		const uint8_t            *mp;
		uint32_t                  mplen = 0;
		struct mat4               model;
		uint32_t                  i;

		if (!w->alive[e] || !(w->mask[e] & COMPONENT_RENDER))
			continue;
		src = (const char *)g_asset_api->get_data(w->render_ref[e], NULL);
		if (!src)
			continue;
		mp   = world_mesh_params(w, e, &mplen);
		blob = mesh_script_generate(src, mp, mplen, g_mem, NULL);
		if (!blob)
			continue;

		mat4_from_transform(&model, &w->world_xform[e]);
		vtx = mesh_blob_vertices(blob);
		idx = mesh_blob_indices(blob);

		for (i = 0; i + 3 <= blob->index_count; i += 3) {
			float a[3], b[3], c[3];
			float t;

			mat4_transform_point(a, &model, vtx[idx[i]].position);
			mat4_transform_point(b, &model, vtx[idx[i + 1]].position);
			mat4_transform_point(c, &model, vtx[idx[i + 2]].position);
			if (ray_tri_intersect(origin, dir, a, b, c, &t) &&
			    t < best_t) {
				best_t = t;
				best   = (int32_t)e;
			}
		}
		g_mem->free(blob);
	}
	return best;
}

/*
 * Left-click over the bare viewport selects the entity under the pointer, or
 * clears the selection on a miss.  Stands down when the gizmo owns the click
 * (a handle grab) or the pointer is over an editor panel, so only clicks that
 * reach the 3D scene ever change the selection.
 *
 * Re-clicking the entity that is already selected doesn't change the
 * selection — instead it cycles the gizmo through Move -> Rotate -> Scale,
 * so repeated clicks step through the transform tools without a trip to
 * the toolbar.
 */
static void pick_update(bool gizmo_active)
{
	float   px, py;
	int32_t hit;

	if (!g_entity_api || !g_kgui)
		return;
	if (gizmo_active || !g_kgui->pointer_clicked())
		return;
	g_kgui->pointer(&px, &py);
	if (g_kgui->over_ui(px, py))
		return;

	hit = pick_entity_at(px, py);
	if (hit != -1 && hit == g_entity_api->get_selected()) {
		g_gizmo_mode = (enum gizmo_mode)((g_gizmo_mode + 1) % 3);
		return;
	}
	g_entity_api->set_selected(hit);
}

/* ------------------------------------------------------------------ */
/* Viewport tools overlay                                              */
/* ------------------------------------------------------------------ */

/*
 * The board header's Undo / Redo buttons and the Play / Pause toggle were lifted
 * onto kruddgui's top toolbar (#492); they draw there now off the krudd-can-undo
 * / krudd-undo / krudd-sim-mode / krudd-toggle-sim accessors above, so their
 * ImGui draw functions (draw_undo_redo / draw_sim_mode) are gone with the board
 * window that hosted them.
 */

/*
 * The editor's viewport-space overlay, registered with kruddgui as an overlay
 * callback (kruddgui_api) and run each kruddgui tick under the panels: keep the
 * camera aspect matched to the live canvas, then draw and drive the selection's
 * transform gizmo and the click-to-pick over the 3D scene, all on kruddgui's own
 * batch and pointer (#492). This is the last ImGui consumer gone from kruddboard;
 * drag-to-spawn (#176), which rode ImGui's drag/drop payloads, is retired with it
 * and will return on a kruddgui drag model if wanted.
 *
 * The board's window chrome is gone (#492): every tab is a standalone kruddgui
 * console (kruddgui.scm), and the header's undo/redo + play/pause moved onto
 * kruddgui's top toolbar. Backtick still toggles g_visible, gating this overlay.
 */
static void draw_viewport_tools(void * /*userdata*/)
{
	float dw, dh;

	if (!g_visible || !g_kgui)
		return;

	g_kgui->viewport(&dw, &dh);
	if (g_camera_api && g_camera_api->set_viewport)
		g_camera_api->set_viewport(dw, dh);
	pick_update(gizmo_update_and_draw());
}

/* ------------------------------------------------------------------ */
/* Plugin lifecycle                                                    */
/* ------------------------------------------------------------------ */

static void kruddboard_init(void)
{
#ifdef __EMSCRIPTEN__
	emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT,
					nullptr, 0, on_keydown);

	/*
	 * Register the Scheme primitives eagerly, not on first panel draw. The
	 * accessors the kruddgui panels read (krudd-gizmo-mode, the toolbar's
	 * krudd-can-undo / krudd-sim-mode, …) live here but drive kruddgui's own
	 * tick, which can run before any kruddboard C panel has drawn. Populating
	 * the shared s7 environment at init makes the bindings exist regardless.
	 */
	ensure_panel_scm();
#endif
	if (g_log)
		g_log->write(LOG_LEVEL_INFO, "kruddboard: init");
}

static void kruddboard_tick(void)
{
#ifdef __EMSCRIPTEN__
	if (g_panels_registered)
		return;

	g_kgui = (const struct kruddgui_api *)
		subsystem_manager_get_api(g_mgr, "kruddgui");

	if (!g_kgui)
		return; /* kruddgui not up yet — retry next tick */

	g_kgui->register_overlay(draw_viewport_tools, nullptr);
	g_panels_registered = 1;

	if (g_log)
		g_log->write(LOG_LEVEL_INFO,
			     "kruddboard: viewport tools registered");
#endif
}

static void kruddboard_shutdown(void)
{
	if (g_log)
		g_log->write(LOG_LEVEL_INFO, "kruddboard: shutdown");
}

static const struct subsystem desc = {
	"kruddboard",
	nullptr,
	kruddboard_init,
	kruddboard_tick,
	kruddboard_shutdown,
};

extern "C" void kruddboard_plugin_entry(struct subsystem_manager *mgr)
{
#ifdef __EMSCRIPTEN__
	g_log       = (const struct log_api *)
		subsystem_manager_get_api(mgr, "log");
	g_stats     = (const struct stats_api *)
		subsystem_manager_get_api(mgr, "stats");
	g_asset_api = (const struct asset_api *)
		subsystem_manager_get_api(mgr, "asset");
	g_asset_mut  = (const struct asset_mut_api *)
		subsystem_manager_get_api(mgr, "asset_mut");
	g_backend    = (const struct backend_api *)
		subsystem_manager_get_api(mgr, "backend");
	g_entity_api = (const struct entity_api *)
		subsystem_manager_get_api(mgr, "scene");
	g_edit_api   = (const struct edit_api *)
		subsystem_manager_get_api(mgr, "edit");
	g_camera_api = (const struct camera_api *)
		subsystem_manager_get_api(mgr, "camera");
	g_preview_api = (const struct preview_api *)
		subsystem_manager_get_api(mgr, "mesh_preview");
	g_mem        = (const struct memory_api *)
		subsystem_manager_get_api(mgr, "memory");
	g_mgr        = mgr;
#endif

	subsystem_manager_register(mgr, &desc);
}
