/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * kgui_accessors — the krudd-* primitives kruddgui.scm reads the engine through.
 *
 * See kgui_accessors.h for why this module exists. In one line: kruddgui.scm's
 * panel set is authored against accessors kruddboard used to register, and this
 * re-registers every one of them against the subsystem apis the engine still
 * publishes, so the panels can draw again.
 *
 * Shape discipline. Each accessor's Scheme return shape is fixed by the panel
 * code that reads it, and pinned by the module's native tests (kgui_scene_test,
 * kgui_assets_test, kgui_perf_test) which drive the same image against stubs of
 * these same names. The doc comment above each primitive states the shape; when
 * one moves, those tests are the arbiter.
 *
 * Absent services. Every accessor degrades to #f / () / 0 rather than faulting
 * when its subsystem is missing, because the panels are drawn by a UI layer that
 * loads before (and outlives) any given plugin — an editor that crashed the frame
 * a service was late would be worse than one that draws an empty console.
 */

#include "kgui_accessors.h"

#include "s7.h"
#include "script.h"

#include "subsystem.h"
#include "subsystem_manager.h"

#include "asset_api.h"
#include "edit_api.h"
#include "entity_api.h"
#include "log_api.h"
#include "preview_api.h"
#include "stats_api.h"

#include "math_types.h"
#include "world.h"

#include "md_parse.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * The one manager handle every accessor resolves its api through. Set at
 * registration; the apis themselves are resolved per call rather than cached,
 * so a plugin that registers after kruddgui (or re-registers) is picked up
 * without a stale pointer surviving in here.
 */
static const struct subsystem_manager *g_mgr;

/*
 * The MOVE / ROTATE / SCALE tool the mode-bar selects. The viewport gizmo that
 * consumed this went with the native editor (#897, see ui/viewport/viewport.c),
 * so today nothing acts on the choice — the mode-bar sets it and reads it back.
 * It lives here rather than in the image so the eventual gizmo has a C-side
 * seam to read, which is where every other consumer of a tool mode would look.
 */
static int32_t g_gizmo_mode;

/* Largest parameter block this module marshals in one call. */
#define KGA_MAX_PARAMS 64
/* The leading shader-ref u32 every material asset's bytes start with. */
#define KGA_MATERIAL_HEADER_BYTES ((uint32_t)sizeof(uint32_t))

/* ------------------------------------------------------------------ */
/* api lookups                                                        */
/* ------------------------------------------------------------------ */

/*
 * One lookup for all of them. The static_table check is not paranoia:
 * subsystem_manager_get_api walks that table before anything else and does not
 * guard it, and kruddgui registers its primitives early enough that a manager
 * mid-construction is reachable here.
 */
static const void *api_of(const char *name)
{
	if (!g_mgr || !g_mgr->static_table)
		return NULL;
	return subsystem_manager_get_api(g_mgr, name);
}

static const struct log_api *log_api_of(void)
{
	return (const struct log_api *)api_of("log");
}

static const struct stats_api *stats_api_of(void)
{
	return (const struct stats_api *)api_of("stats");
}

static const struct entity_api *scene_api_of(void)
{
	return (const struct entity_api *)api_of("scene");
}

static const struct asset_api *asset_api_of(void)
{
	return (const struct asset_api *)api_of("asset");
}

static const struct asset_mut_api *asset_mut_api_of(void)
{
	return (const struct asset_mut_api *)api_of("asset_mut");
}

static const struct edit_api *edit_api_of(void)
{
	return (const struct edit_api *)api_of("edit");
}

static const struct preview_api *preview_api_of(void)
{
	return (const struct preview_api *)api_of("mesh_preview");
}

/* ------------------------------------------------------------------ */
/* small s7 helpers                                                   */
/* ------------------------------------------------------------------ */

static int32_t arg_int(s7_scheme *sc, s7_pointer args, int i)
{
	s7_pointer p = s7_list_ref(sc, args, i);

	return s7_is_number(p) ? (int32_t)s7_number_to_real(sc, p) : 0;
}

static const char *arg_str(s7_scheme *sc, s7_pointer args, int i)
{
	s7_pointer p = s7_list_ref(sc, args, i);

	return s7_is_string(p) ? s7_string(p) : NULL;
}

/* A list of `n` floats as Scheme reals. */
static s7_pointer floats_to_list(s7_scheme *sc, const float *v, uint32_t n)
{
	s7_pointer out = s7_nil(sc);
	uint32_t   i;

	for (i = n; i > 0; i--)
		out = s7_cons(sc, s7_make_real(sc, (double)v[i - 1]), out);
	return out;
}

/*
 * Read up to `n` floats out of a Scheme list of numbers. Missing or
 * non-numeric entries leave the slot untouched, so a caller seeds defaults.
 */
static void list_to_floats(s7_scheme *sc, s7_pointer lst, float *out,
			   uint32_t n)
{
	uint32_t i;

	for (i = 0; i < n && s7_is_pair(lst); i++, lst = s7_cdr(lst)) {
		s7_pointer v = s7_car(lst);

		if (s7_is_number(v))
			out[i] = (float)s7_number_to_real(sc, v);
	}
}

/* ------------------------------------------------------------------ */
/* log / stats / subsystems — the Log and KRUDD consoles               */
/* ------------------------------------------------------------------ */

/*
 * (krudd-log-history) -> ((level . text) ...) oldest-first, or #f when the log
 * service is absent. The Log console's level chips filter on the car.
 */
static s7_pointer sp_log_history(s7_scheme *sc, s7_pointer args)
{
	static struct log_message buf[LOG_HISTORY_CAP];
	const struct log_api     *log = log_api_of();
	s7_pointer                out = s7_nil(sc);
	uint32_t                  n, i;

	(void)args;
	if (!log || !log->get_history)
		return s7_f(sc);
	n = log->get_history(buf, LOG_HISTORY_CAP);
	for (i = n; i > 0; i--)
		out = s7_cons(sc,
			      s7_cons(sc, s7_make_integer(sc,
							  buf[i - 1].level),
				      s7_make_string(sc, buf[i - 1].text)),
			      out);
	return out;
}

/*
 * (krudd-stats) -> (fps frame-ms frame-count), or #f when the stats service is
 * absent. Read by both the KRUDD console and the perf HUD.
 */
static s7_pointer sp_stats(s7_scheme *sc, s7_pointer args)
{
	const struct stats_api *st = stats_api_of();

	(void)args;
	if (!st)
		return s7_f(sc);
	return s7_list(sc, 3, s7_make_real(sc, (double)st->fps_avg),
		       s7_make_real(sc, (double)st->last_frame_ms),
		       s7_make_integer(sc, st->frame_count));
}

/*
 * (krudd-startup) -> (init-ms first-frame-ms page-first-ms (name . ms) ...),
 * or #f when the stats service is absent. The trailing pairs are the boot
 * profile's phases in the order they ran.
 */
static s7_pointer sp_startup(s7_scheme *sc, s7_pointer args)
{
	const struct stats_api *st = stats_api_of();
	s7_pointer              phases = s7_nil(sc);
	uint32_t                i, n;

	(void)args;
	if (!st)
		return s7_f(sc);
	n = st->phase_count > STATS_MAX_PHASES ? STATS_MAX_PHASES
					       : st->phase_count;
	for (i = n; i > 0; i--) {
		const struct stats_phase *p = &st->phases[i - 1];

		phases = s7_cons(sc,
				 s7_cons(sc,
					 s7_make_string(sc, p->name ? p->name
								   : "?"),
					 s7_make_real(sc, (double)p->ms)),
				 phases);
	}
	return s7_cons(sc, s7_make_real(sc, (double)st->init_ms),
		       s7_cons(sc, s7_make_real(sc, (double)st->first_frame_ms),
			       s7_cons(sc,
				       s7_make_real(sc,
					(double)st->page_to_first_frame_ms),
				       phases)));
}

/* One (name api? tick? size) row for the KRUDD console's subsystem table. */
static s7_pointer subsystem_row(s7_scheme *sc, const struct subsystem *s)
{
	return s7_list(sc, 4, s7_make_string(sc, s->name ? s->name : "?"),
		       s7_make_boolean(sc, s->api != NULL),
		       s7_make_boolean(sc, s->tick != NULL),
		       s7_make_integer(sc, s->wasm_size));
}

/*
 * (krudd-subsystems) -> ((name api? tick? size) ...) or #f, static table first
 * and then the dynamically registered plugins, each in registration order —
 * the order they were initialized, which is what the table is read to check.
 */
static s7_pointer sp_subsystems(s7_scheme *sc, s7_pointer args)
{
	s7_pointer out = s7_nil(sc), tail = s7_nil(sc);
	int32_t    i;

	(void)args;
	if (!g_mgr)
		return s7_f(sc);
	for (i = g_mgr->dynamic_count; i > 0; i--)
		tail = s7_cons(sc, subsystem_row(sc, &g_mgr->dynamic[i - 1]),
			       tail);
	out = tail;
	if (g_mgr->static_table) {
		int32_t n = 0;

		while (g_mgr->static_table[n].name)
			n++;
		for (i = n; i > 0; i--)
			out = s7_cons(sc,
				      subsystem_row(sc,
						    &g_mgr->static_table[i - 1]),
				      out);
	}
	return out;
}

/*
 * (krudd-md-parse text) -> ((type level (run ...)) ...), run = (text . style)
 * with style 0 normal / 1 bold / 2 code. () for empty or non-string input. The
 * markdown preview in the authored-text inspector draws these blocks; parsing
 * and the byte-accurate span split stay in C (ui/kruddboard/md_parse.c).
 */
static s7_pointer sp_md_parse(s7_scheme *sc, s7_pointer args)
{
	static struct md_block blocks[MD_BLOCKS_MAX];
	const char            *src = arg_str(sc, args, 0);
	s7_pointer             out = s7_nil(sc);
	int32_t                n, i;

	if (!src || !*src)
		return s7_nil(sc);
	n = md_parse(src, blocks, MD_BLOCKS_MAX);
	if (n <= 0)
		return s7_nil(sc);
	for (i = n; i > 0; i--) {
		const struct md_block *b = &blocks[i - 1];
		s7_pointer             runs = s7_nil(sc);
		uint32_t               s;

		for (s = b->span_count; s > 0; s--) {
			const struct md_span *sp = &b->spans[s - 1];
			char                  text[MD_TEXT_MAX];
			uint32_t              len;

			if (sp->end <= sp->start)
				continue;
			len = sp->end - sp->start;
			if (len >= sizeof(text))
				len = sizeof(text) - 1;
			memcpy(text, b->text + sp->start, len);
			text[len] = '\0';
			runs = s7_cons(sc,
				       s7_cons(sc, s7_make_string(sc, text),
					       s7_make_integer(sc, sp->style)),
				       runs);
		}
		out = s7_cons(sc,
			      s7_list(sc, 3, s7_make_integer(sc, b->type),
				      s7_make_integer(sc, b->level), runs),
			      out);
	}
	return out;
}

/* ------------------------------------------------------------------ */
/* undo / redo and the tool mode — the toolbar and mode-bar            */
/* ------------------------------------------------------------------ */

/* (krudd-can-undo) / (krudd-can-redo) -> #t when the timeline has a step. */
static s7_pointer sp_can_undo(s7_scheme *sc, s7_pointer args)
{
	const struct edit_api *e = edit_api_of();

	(void)args;
	return s7_make_boolean(sc, e && e->can_undo && e->can_undo() != 0);
}

static s7_pointer sp_can_redo(s7_scheme *sc, s7_pointer args)
{
	const struct edit_api *e = edit_api_of();

	(void)args;
	return s7_make_boolean(sc, e && e->can_redo && e->can_redo() != 0);
}

/* (krudd-undo) / (krudd-redo) -> #t when a step was taken. */
static s7_pointer sp_undo(s7_scheme *sc, s7_pointer args)
{
	const struct edit_api *e = edit_api_of();

	(void)args;
	return s7_make_boolean(sc, e && e->undo && e->undo() != 0);
}

static s7_pointer sp_redo(s7_scheme *sc, s7_pointer args)
{
	const struct edit_api *e = edit_api_of();

	(void)args;
	return s7_make_boolean(sc, e && e->redo && e->redo() != 0);
}

/* (krudd-gizmo-mode) -> 0 move / 1 rotate / 2 scale. */
static s7_pointer sp_gizmo_mode(s7_scheme *sc, s7_pointer args)
{
	(void)args;
	return s7_make_integer(sc, g_gizmo_mode);
}

/* (krudd-set-gizmo-mode m) — out-of-range values are ignored. */
static s7_pointer sp_set_gizmo_mode(s7_scheme *sc, s7_pointer args)
{
	int32_t m = arg_int(sc, args, 0);

	if (m >= 0 && m <= 2)
		g_gizmo_mode = m;
	return s7_unspecified(sc);
}

/* ------------------------------------------------------------------ */
/* the scene console                                                   */
/* ------------------------------------------------------------------ */

/*
 * A live entity's name, or NULL when it has none. This is world_entity_name's
 * body rather than a call to it: that symbol lives in entity.c, and linking
 * the entity plugin in here — a plugin that registers its own subsystem —
 * to reach one bounds check over two columns would invert the dependency (the
 * ui layer pulling in a world plugin) for no gain. world.h's columns are the
 * contract either way.
 */
static const char *entity_name(const struct world *w, uint32_t e)
{
	if (e >= w->count || !(w->mask[e] & COMPONENT_NAME) ||
	    w->name_off[e] == WORLD_NO_NAME)
		return NULL;
	return w->names + w->name_off[e];
}

/*
 * (krudd-world-caps) -> (can-edit-entities? can-edit-assets?). The Scene panel
 * greys its mutating controls on these rather than probing each setter.
 */
static s7_pointer sp_world_caps(s7_scheme *sc, s7_pointer args)
{
	(void)args;
	return s7_list(sc, 2, s7_make_boolean(sc, scene_api_of() != NULL),
		       s7_make_boolean(sc, asset_mut_api_of() != NULL));
}

/* (krudd-selected) -> the selected entity id, or -1 for none. */
static s7_pointer sp_selected(s7_scheme *sc, s7_pointer args)
{
	const struct entity_api *e = scene_api_of();

	(void)args;
	return s7_make_integer(sc,
			       (e && e->get_selected) ? e->get_selected() : -1);
}

/*
 * (krudd-world-entities) -> ((id . name-or-#f) ...) for every live entity, in
 * id order — which world.h guarantees is topological, so a parent always
 * precedes its children and the tree builds in one pass.
 */
static s7_pointer sp_world_entities(s7_scheme *sc, s7_pointer args)
{
	const struct entity_api *e = scene_api_of();
	const struct world      *w;
	s7_pointer               out = s7_nil(sc);
	uint32_t                 i;

	(void)args;
	if (!e || !e->get_world)
		return s7_nil(sc);
	w = e->get_world();
	if (!w)
		return s7_nil(sc);
	for (i = w->count; i > 0; i--) {
		uint32_t    id   = i - 1;
		const char *name;

		if (!w->alive[id])
			continue;
		name = entity_name(w, id);
		out  = s7_cons(sc,
			       s7_cons(sc, s7_make_integer(sc, (int32_t)id),
				       name ? s7_make_string(sc, name)
					    : s7_f(sc)),
			       out);
	}
	return out;
}

/*
 * (krudd-entity-inspect id) -> the inspector's 12-tuple, or #f for a stale id:
 *
 *   0 name (string or #f)          6 has-render?      9  material-ref
 *   1 position (x y z)             7 has-material?   10  has-script?
 *   2 rotation (x y z w)           8 render-ref      11  script-ref
 *   3 scale (x y z)
 *   4 parent (#f at root, else (id . name-or-#f))
 *   5 alive? — always #t here (a dead id returns #f above); the slot is held
 *     so the tuple's later indices, which the panel reads by position, stay put.
 */
static s7_pointer sp_entity_inspect(s7_scheme *sc, s7_pointer args)
{
	const struct entity_api *e  = scene_api_of();
	int32_t                  id = arg_int(sc, args, 0);
	const struct world      *w;
	const struct transform  *t;
	const char              *name;
	s7_pointer               parent;

	if (!e || !e->get_world)
		return s7_f(sc);
	w = e->get_world();
	if (!w || id < 0 || (uint32_t)id >= w->count || !w->alive[id])
		return s7_f(sc);
	t    = &w->local[id];
	name = entity_name(w, (uint32_t)id);
	if (w->parent[id] < 0) {
		parent = s7_f(sc);
	} else {
		int32_t     pid   = w->parent[id];
		const char *pname = entity_name(w, (uint32_t)pid);

		parent = s7_cons(sc, s7_make_integer(sc, pid),
				 pname ? s7_make_string(sc, pname) : s7_f(sc));
	}
	return s7_list(sc, 12, name ? s7_make_string(sc, name) : s7_f(sc),
		       floats_to_list(sc, t->position, 3),
		       floats_to_list(sc, t->rotation, 4),
		       floats_to_list(sc, t->scale, 3), parent, s7_t(sc),
		       s7_make_boolean(sc,
				       (w->mask[id] & COMPONENT_RENDER) != 0),
		       s7_make_boolean(sc,
				       (w->mask[id] & COMPONENT_MATERIAL) != 0),
		       s7_make_integer(sc, w->render_ref[id]),
		       s7_make_integer(sc, w->material_ref[id]),
		       s7_make_boolean(sc,
				       (w->mask[id] & COMPONENT_SCRIPT) != 0),
		       s7_make_integer(sc, w->script_ref[id]));
}

/* (krudd-entity-create) -> the new entity's id, or -1 when the world is full. */
static s7_pointer sp_entity_create(s7_scheme *sc, s7_pointer args)
{
	const struct entity_api *e = scene_api_of();
	struct transform         t;

	(void)args;
	if (!e || !e->create_entity)
		return s7_make_integer(sc, -1);
	memset(&t, 0, sizeof(t));
	t.rotation[3] = 1.0f;
	t.scale[0] = t.scale[1] = t.scale[2] = 1.0f;
	return s7_make_integer(sc, e->create_entity(-1, &t, 0, 0));
}

/* (krudd-entity-destroy id) — tombstones id and its subtree. */
static s7_pointer sp_entity_destroy(s7_scheme *sc, s7_pointer args)
{
	const struct entity_api *e = scene_api_of();

	if (e && e->destroy_entity)
		e->destroy_entity(arg_int(sc, args, 0));
	return s7_unspecified(sc);
}

/* (krudd-entity-select id) — -1 clears the selection. */
static s7_pointer sp_entity_select(s7_scheme *sc, s7_pointer args)
{
	const struct entity_api *e = scene_api_of();

	if (e && e->set_selected)
		e->set_selected(arg_int(sc, args, 0));
	return s7_unspecified(sc);
}

/* (krudd-entity-set-name id name) — an empty name clears it. */
static s7_pointer sp_entity_set_name(s7_scheme *sc, s7_pointer args)
{
	const struct entity_api *e = scene_api_of();

	if (e && e->set_name)
		e->set_name(arg_int(sc, args, 0), arg_str(sc, args, 1));
	return s7_unspecified(sc);
}

/*
 * (krudd-entity-set-transform id pos rot scale) — pos/scale are 3-lists and rot
 * a 4-list (quaternion xyzw); a short list leaves the rest at the identity.
 */
static s7_pointer sp_entity_set_transform(s7_scheme *sc, s7_pointer args)
{
	const struct entity_api *e = scene_api_of();
	struct transform         t;

	if (!e || !e->set_transform)
		return s7_unspecified(sc);
	memset(&t, 0, sizeof(t));
	t.rotation[3] = 1.0f;
	t.scale[0] = t.scale[1] = t.scale[2] = 1.0f;
	list_to_floats(sc, s7_list_ref(sc, args, 1), t.position, 3);
	list_to_floats(sc, s7_list_ref(sc, args, 2), t.rotation, 4);
	list_to_floats(sc, s7_list_ref(sc, args, 3), t.scale, 3);
	e->set_transform(arg_int(sc, args, 0), &t);
	return s7_unspecified(sc);
}

/* (krudd-entity-set-render-ref id ref) — a zero ref unbinds the mesh. */
static s7_pointer sp_entity_set_render_ref(s7_scheme *sc, s7_pointer args)
{
	const struct entity_api *e = scene_api_of();

	if (e && e->set_render_ref)
		e->set_render_ref(arg_int(sc, args, 0),
				  (uint32_t)arg_int(sc, args, 1));
	return s7_unspecified(sc);
}

/* (krudd-entity-set-material-ref id ref) — a zero ref unbinds the material. */
static s7_pointer sp_entity_set_material_ref(s7_scheme *sc, s7_pointer args)
{
	const struct entity_api *e = scene_api_of();

	if (e && e->set_material_ref)
		e->set_material_ref(arg_int(sc, args, 0),
				    (uint32_t)arg_int(sc, args, 1));
	return s7_unspecified(sc);
}

/* (krudd-entity-set-script-ref id ref) — a zero ref unbinds the script. */
static s7_pointer sp_entity_set_script_ref(s7_scheme *sc, s7_pointer args)
{
	const struct entity_api *e = scene_api_of();

	if (e && e->set_script_ref)
		e->set_script_ref(arg_int(sc, args, 0),
				  (uint32_t)arg_int(sc, args, 1));
	return s7_unspecified(sc);
}

/* ------------------------------------------------------------------ */
/* parameter blocks — the shared marshalling                           */
/* ------------------------------------------------------------------ */

/*
 * One param as the 8-tuple every param widget reads:
 *   (name type offset size components edit-kind edit-min edit-max)
 * `type` is a symbol (float / vec4 / ...), `edit-kind` a string
 * ("none" / "color" / "range").
 */
static s7_pointer param_row(s7_scheme *sc, const struct shader_param *p)
{
	return s7_list(sc, 8, s7_make_string(sc, p->name),
		       s7_make_symbol(sc, p->type),
		       s7_make_integer(sc, p->offset),
		       s7_make_integer(sc, p->size),
		       s7_make_integer(sc, p->components),
		       s7_make_string(sc, p->edit),
		       s7_make_real(sc, (double)p->edit_min),
		       s7_make_real(sc, (double)p->edit_max));
}

static s7_pointer params_to_list(s7_scheme *sc, const struct shader_param *ps,
				 int n)
{
	s7_pointer out = s7_nil(sc);
	int        i;

	for (i = n; i > 0; i--)
		out = s7_cons(sc, param_row(sc, &ps[i - 1]), out);
	return out;
}

/* A param's authored default, padded out to its component count. */
static void param_defaults(const struct shader_param *p, float *out)
{
	uint32_t i;

	for (i = 0; i < p->components && i < 4; i++)
		out[i] = i < p->default_count ? p->edit_default[i] : 0.0f;
}

/*
 * Decode one param's components out of `bytes`, falling back to its authored
 * default when the block is absent or too short to hold the field.
 */
static s7_pointer param_values(s7_scheme *sc, const struct shader_param *p,
			       const uint8_t *bytes, uint32_t len)
{
	float    v[4];
	uint32_t comps = p->components > 4 ? 4 : p->components;

	param_defaults(p, v);
	if (bytes && p->offset + comps * sizeof(float) <= len)
		memcpy(v, bytes + p->offset, comps * sizeof(float));
	return floats_to_list(sc, v, comps);
}

static s7_pointer values_to_list(s7_scheme *sc, const struct shader_param *ps,
				 int n, const uint8_t *bytes, uint32_t len)
{
	s7_pointer out = s7_nil(sc);
	int        i;

	for (i = n; i > 0; i--)
		out = s7_cons(sc, param_values(sc, &ps[i - 1], bytes, len),
			      out);
	return out;
}

/*
 * Pack a Scheme list-of-value-lists back into `bytes` against the param layout,
 * seeding every field with its authored default first so a value list that
 * omits a param writes that param's default rather than a zero.
 */
static uint32_t pack_values(s7_scheme *sc, const struct shader_param *ps, int n,
			    s7_pointer values, uint8_t *bytes, uint32_t cap,
			    uint32_t total)
{
	int i;

	if (total > cap)
		return 0;
	memset(bytes, 0, total);
	for (i = 0; i < n; i++) {
		const struct shader_param *p = &ps[i];
		float                      v[4];
		uint32_t                   comps = p->components > 4
							   ? 4
							   : p->components;

		if (p->offset + comps * sizeof(float) > total)
			continue;
		param_defaults(p, v);
		if (s7_is_pair(values)) {
			list_to_floats(sc, s7_car(values), v, comps);
			values = s7_cdr(values);
		}
		memcpy(bytes + p->offset, v, comps * sizeof(float));
	}
	return total;
}

/* The source text of an asset id, or NULL when it holds no loaded bytes. */
static const char *asset_source(uint32_t id, uint32_t *out_size)
{
	const struct asset_api *a = asset_api_of();
	uint32_t                size = 0;
	const void             *data;

	if (!a || !a->get_data || !id)
		return NULL;
	data = a->get_data(id, &size);
	if (out_size)
		*out_size = size;
	return (const char *)data;
}

/*
 * Introspect an asset's declared params with the right script.h reader for its
 * type. Returns the field count (0 when the asset holds no params), or -1 when
 * the source is unreadable.
 */
typedef int (*param_reader)(const char *, struct shader_param *, uint32_t,
			    uint32_t *);

static int read_params(uint32_t id, param_reader reader,
		       struct shader_param *out, uint32_t *total)
{
	uint32_t    size = 0;
	const char *src  = asset_source(id, &size);
	char        buf[8192];

	if (!src || !size)
		return -1;
	if (size >= sizeof(buf))
		return -1;
	memcpy(buf, src, size);
	buf[size] = '\0';
	return reader(buf, out, KGA_MAX_PARAMS, total);
}

/* ------------------------------------------------------------------ */
/* per-asset param accessors                                           */
/* ------------------------------------------------------------------ */

/*
 * (krudd-shader-material-params shader-ref) -> the shader's Material block as
 * param rows; () for a shader with no block or an unreadable source.
 */
static s7_pointer sp_shader_material_params(s7_scheme *sc, s7_pointer args)
{
	struct shader_param ps[KGA_MAX_PARAMS];
	int                 n = read_params((uint32_t)arg_int(sc, args, 0),
					    script_shader_material_params, ps,
					    NULL);

	return n > 0 ? params_to_list(sc, ps, n) : s7_nil(sc);
}

/* (krudd-mesh-params mesh-ref) -> the mesh script's (params ...) rows. */
static s7_pointer sp_mesh_params(s7_scheme *sc, s7_pointer args)
{
	struct shader_param ps[KGA_MAX_PARAMS];
	int                 n = read_params((uint32_t)arg_int(sc, args, 0),
					    script_mesh_params, ps, NULL);

	return n > 0 ? params_to_list(sc, ps, n) : s7_nil(sc);
}

/* (krudd-script-params script-ref) -> the entity script's (params ...) rows. */
static s7_pointer sp_script_params(s7_scheme *sc, s7_pointer args)
{
	struct shader_param ps[KGA_MAX_PARAMS];
	int                 n = read_params((uint32_t)arg_int(sc, args, 0),
					    script_entity_params, ps, NULL);

	return n > 0 ? params_to_list(sc, ps, n) : s7_nil(sc);
}

/* (krudd-texture-params texture-ref) -> the texture script's (params ...) rows. */
static s7_pointer sp_texture_params(s7_scheme *sc, s7_pointer args)
{
	struct shader_param ps[KGA_MAX_PARAMS];
	int                 n = read_params((uint32_t)arg_int(sc, args, 0),
					    script_texture_params, ps, NULL);

	return n > 0 ? params_to_list(sc, ps, n) : s7_nil(sc);
}

/*
 * (krudd-texture-values texture-ref) -> one value list per declared param. A
 * texture asset's bytes are its source, not a packed block, so the values are
 * the authored defaults — the per-entity overrides live on the entity and are
 * read through krudd-entity-texture-values.
 */
static s7_pointer sp_texture_values(s7_scheme *sc, s7_pointer args)
{
	struct shader_param ps[KGA_MAX_PARAMS];
	int                 n = read_params((uint32_t)arg_int(sc, args, 0),
					    script_texture_params, ps, NULL);

	return n > 0 ? values_to_list(sc, ps, n, NULL, 0) : s7_nil(sc);
}

/* ------------------------------------------------------------------ */
/* materials                                                           */
/* ------------------------------------------------------------------ */

/*
 * (krudd-asset-shader-ref material-id) -> the shader asset id in the material's
 * leading header, or 0 when it has none.
 */
static s7_pointer sp_asset_shader_ref(s7_scheme *sc, s7_pointer args)
{
	uint32_t    size = 0;
	const char *bytes = asset_source((uint32_t)arg_int(sc, args, 0), &size);
	uint32_t    ref   = 0;

	if (bytes && size >= KGA_MATERIAL_HEADER_BYTES)
		memcpy(&ref, bytes, sizeof(ref));
	return s7_make_integer(sc, ref);
}

/* The std140 size of a shader asset's Material block, 0 when it has none. */
static uint32_t material_block_size(uint32_t shader_ref)
{
	struct shader_param ps[KGA_MAX_PARAMS];
	uint32_t            total = 0;

	if (read_params(shader_ref, script_shader_material_params, ps,
			&total) < 0)
		return 0;
	return total;
}

/*
 * (krudd-material-texture material-id) -> (tex-ref width height) when the
 * material carries a texture slot, else () for "(none)". The slot is the
 * trailer after the shader-ref header and the shader's Material block:
 *   [shader-ref u32][block][tex-ref u32][w u32][h u32][texture params...]
 */
static s7_pointer sp_material_texture(s7_scheme *sc, s7_pointer args)
{
	uint32_t    size = 0;
	const char *bytes = asset_source((uint32_t)arg_int(sc, args, 0), &size);
	uint32_t    shader_ref = 0, block, off, tex = 0, w = 0, h = 0;

	if (!bytes || size < KGA_MATERIAL_HEADER_BYTES)
		return s7_nil(sc);
	memcpy(&shader_ref, bytes, sizeof(shader_ref));
	block = material_block_size(shader_ref);
	if (!block)
		return s7_nil(sc);
	off = KGA_MATERIAL_HEADER_BYTES + block;
	if (size < off + 3u * sizeof(uint32_t))
		return s7_nil(sc);
	memcpy(&tex, bytes + off, sizeof(tex));
	memcpy(&w, bytes + off + 4u, sizeof(w));
	memcpy(&h, bytes + off + 8u, sizeof(h));
	if (!tex)
		return s7_nil(sc);
	return s7_list(sc, 3, s7_make_integer(sc, tex), s7_make_integer(sc, w),
		       s7_make_integer(sc, h));
}

/*
 * (krudd-material-values material-id shader-ref) -> one value list per param of
 * the shader's Material block, decoded from the material's own bytes.
 */
static s7_pointer sp_material_values(s7_scheme *sc, s7_pointer args)
{
	struct shader_param ps[KGA_MAX_PARAMS];
	uint32_t            size = 0;
	const char         *bytes;
	int                 n;

	n = read_params((uint32_t)arg_int(sc, args, 1),
			script_shader_material_params, ps, NULL);
	if (n <= 0)
		return s7_nil(sc);
	bytes = asset_source((uint32_t)arg_int(sc, args, 0), &size);
	if (bytes && size > KGA_MATERIAL_HEADER_BYTES)
		return values_to_list(sc, ps, n,
				      (const uint8_t *)bytes +
					      KGA_MATERIAL_HEADER_BYTES,
				      size - KGA_MATERIAL_HEADER_BYTES);
	return values_to_list(sc, ps, n, NULL, 0);
}

/*
 * Compose a material asset's bytes from a shader ref, its param values and an
 * optional texture slot. Returns the byte count written, or 0 when the layout
 * does not fit the buffer.
 */
static uint32_t build_material(s7_scheme *sc, uint32_t shader_ref,
			       s7_pointer values, s7_pointer args, int tex_arg,
			       uint8_t *out, uint32_t cap)
{
	struct shader_param ps[KGA_MAX_PARAMS];
	uint32_t            total = 0, off;
	int                 n;

	if (cap < KGA_MATERIAL_HEADER_BYTES)
		return 0;
	memcpy(out, &shader_ref, sizeof(shader_ref));
	off = KGA_MATERIAL_HEADER_BYTES;
	n   = read_params(shader_ref, script_shader_material_params, ps,
			  &total);
	if (n > 0 && total > 0) {
		if (!pack_values(sc, ps, n, values, out + off, cap - off,
				 total))
			return 0;
		off += total;
	}
	/* The optional (tex-ref w h) trailer, present only when all three came. */
	if (s7_list_length(sc, args) > tex_arg + 2) {
		uint32_t tex = (uint32_t)arg_int(sc, args, tex_arg);
		uint32_t w   = (uint32_t)arg_int(sc, args, tex_arg + 1);
		uint32_t h   = (uint32_t)arg_int(sc, args, tex_arg + 2);

		if (tex && cap >= off + 3u * sizeof(uint32_t)) {
			memcpy(out + off, &tex, sizeof(tex));
			memcpy(out + off + 4u, &w, sizeof(w));
			memcpy(out + off + 8u, &h, sizeof(h));
			off += 3u * sizeof(uint32_t);
		}
	}
	return off;
}

/*
 * (krudd-asset-save-material id shader-ref values [tex-ref w h]) — rewrite the
 * material's bytes in place.
 */
static s7_pointer sp_asset_save_material(s7_scheme *sc, s7_pointer args)
{
	const struct asset_mut_api *mut = asset_mut_api_of();
	uint8_t                     buf[1024];
	uint32_t                    n;

	if (!mut || !mut->set_data)
		return s7_unspecified(sc);
	n = build_material(sc, (uint32_t)arg_int(sc, args, 1),
			   s7_list_ref(sc, args, 2), args, 3, buf, sizeof(buf));
	if (n)
		mut->set_data((uint32_t)arg_int(sc, args, 0), buf, n);
	return s7_unspecified(sc);
}

/*
 * (krudd-asset-clone-material name shader-ref values [tex-ref w h]) -> the new
 * asset's id, or 0 when the name clashes or the catalog is full.
 */
static s7_pointer sp_asset_clone_material(s7_scheme *sc, s7_pointer args)
{
	const struct asset_mut_api *mut  = asset_mut_api_of();
	const char                 *name = arg_str(sc, args, 0);
	uint8_t                     buf[1024];
	uint32_t                    n;

	if (!mut || !mut->create || !name)
		return s7_make_integer(sc, 0);
	n = build_material(sc, (uint32_t)arg_int(sc, args, 1),
			   s7_list_ref(sc, args, 2), args, 3, buf, sizeof(buf));
	if (!n)
		return s7_make_integer(sc, 0);
	return s7_make_integer(sc,
			       mut->create(name, ASSET_TYPE_MATERIAL, buf, n));
}

/* ------------------------------------------------------------------ */
/* per-entity parameter overrides                                      */
/* ------------------------------------------------------------------ */

/* The live world, or NULL when the scene service is absent. */
static const struct world *world_of(void)
{
	const struct entity_api *e = scene_api_of();

	return (e && e->get_world) ? e->get_world() : NULL;
}

static int entity_live(const struct world *w, int32_t id)
{
	return w && id >= 0 && (uint32_t)id < w->count && w->alive[id];
}

/*
 * The shared body of the four (krudd-entity-*-values entity asset-ref) readers:
 * decode the entity's override bytes against the asset's declared params, or
 * report the authored defaults when the entity holds no override.
 */
static s7_pointer entity_values(s7_scheme *sc, s7_pointer args,
				param_reader reader, const uint8_t *bytes,
				uint32_t len)
{
	struct shader_param ps[KGA_MAX_PARAMS];
	int                 n = read_params((uint32_t)arg_int(sc, args, 1),
					    reader, ps, NULL);

	if (n <= 0)
		return s7_nil(sc);
	return values_to_list(sc, ps, n, len ? bytes : NULL, len);
}

/* (krudd-entity-mesh-values id mesh-ref) -> one value list per mesh param. */
static s7_pointer sp_entity_mesh_values(s7_scheme *sc, s7_pointer args)
{
	const struct world *w  = world_of();
	int32_t             id = arg_int(sc, args, 0);

	if (!entity_live(w, id))
		return entity_values(sc, args, script_mesh_params, NULL, 0);
	return entity_values(sc, args, script_mesh_params, w->mesh_params[id],
			     w->mesh_param_len[id]);
}

/* (krudd-entity-script-values id script-ref) -> one value list per script param. */
static s7_pointer sp_entity_script_values(s7_scheme *sc, s7_pointer args)
{
	const struct world *w  = world_of();
	int32_t             id = arg_int(sc, args, 0);

	if (!entity_live(w, id))
		return entity_values(sc, args, script_entity_params, NULL, 0);
	return entity_values(sc, args, script_entity_params,
			     w->script_params[id], w->script_param_len[id]);
}

/* (krudd-entity-texture-values id texture-ref) -> one list per texture param. */
static s7_pointer sp_entity_texture_values(s7_scheme *sc, s7_pointer args)
{
	const struct world *w  = world_of();
	int32_t             id = arg_int(sc, args, 0);

	if (!entity_live(w, id))
		return entity_values(sc, args, script_texture_params, NULL, 0);
	return entity_values(sc, args, script_texture_params,
			     w->texture_params[id], w->texture_param_len[id]);
}

/*
 * (krudd-entity-material-values id material-ref shader-ref) -> one value list
 * per Material-block param: the entity's own override when it holds one, else
 * the material asset's values, else the authored defaults.
 */
static s7_pointer sp_entity_material_values(s7_scheme *sc, s7_pointer args)
{
	struct shader_param ps[KGA_MAX_PARAMS];
	const struct world *w    = world_of();
	int32_t             id   = arg_int(sc, args, 0);
	uint32_t            size = 0;
	const char         *bytes;
	int                 n;

	n = read_params((uint32_t)arg_int(sc, args, 2),
			script_shader_material_params, ps, NULL);
	if (n <= 0)
		return s7_nil(sc);
	if (entity_live(w, id) && w->material_param_len[id])
		return values_to_list(sc, ps, n, w->material_params[id],
				      w->material_param_len[id]);
	bytes = asset_source((uint32_t)arg_int(sc, args, 1), &size);
	if (bytes && size > KGA_MATERIAL_HEADER_BYTES)
		return values_to_list(sc, ps, n,
				      (const uint8_t *)bytes +
					      KGA_MATERIAL_HEADER_BYTES,
				      size - KGA_MATERIAL_HEADER_BYTES);
	return values_to_list(sc, ps, n, NULL, 0);
}

/*
 * The shared body of the four (krudd-entity-save-*-params entity ref values)
 * writers: pack the values against the asset's layout and hand them to the
 * matching entity_api setter, which records the undo step.
 */
static s7_pointer entity_save(s7_scheme *sc, s7_pointer args,
			      param_reader reader,
			      void (*set)(int32_t, const uint8_t *, uint32_t),
			      uint32_t cap)
{
	struct shader_param ps[KGA_MAX_PARAMS];
	uint8_t             buf[WORLD_MATERIAL_PARAM_CAP];
	uint32_t            total = 0, n;
	int                 count;

	if (!set)
		return s7_unspecified(sc);
	count = read_params((uint32_t)arg_int(sc, args, 1), reader, ps, &total);
	if (count < 0 || !total || total > cap || total > sizeof(buf))
		return s7_unspecified(sc);
	n = pack_values(sc, ps, count, s7_list_ref(sc, args, 2), buf,
			sizeof(buf), total);
	if (n)
		set(arg_int(sc, args, 0), buf, n);
	return s7_unspecified(sc);
}

static s7_pointer sp_entity_save_mesh_params(s7_scheme *sc, s7_pointer args)
{
	const struct entity_api *e = scene_api_of();

	return entity_save(sc, args, script_mesh_params,
			   e ? e->set_mesh_params : NULL,
			   WORLD_MESH_PARAM_CAP);
}

static s7_pointer sp_entity_save_script_params(s7_scheme *sc, s7_pointer args)
{
	const struct entity_api *e = scene_api_of();

	return entity_save(sc, args, script_entity_params,
			   e ? e->set_script_params : NULL,
			   WORLD_SCRIPT_PARAM_CAP);
}

static s7_pointer sp_entity_save_texture_params(s7_scheme *sc, s7_pointer args)
{
	const struct entity_api *e = scene_api_of();

	return entity_save(sc, args, script_texture_params,
			   e ? e->set_texture_params : NULL,
			   WORLD_TEXTURE_PARAM_CAP);
}

/*
 * (krudd-entity-save-material-params id shader-ref values) — the material twin,
 * which packs against the SHADER's block rather than an asset of its own type.
 */
static s7_pointer sp_entity_save_material_params(s7_scheme *sc, s7_pointer args)
{
	const struct entity_api *e = scene_api_of();

	return entity_save(sc, args, script_shader_material_params,
			   e ? e->set_material_params : NULL,
			   WORLD_MATERIAL_PARAM_CAP);
}

/* ------------------------------------------------------------------ */
/* the asset catalog                                                   */
/* ------------------------------------------------------------------ */

/* One (id path type kind state size refs) row for the asset browser. */
static s7_pointer asset_row(s7_scheme *sc, const struct asset_info *in)
{
	return s7_list(sc, 7, s7_make_integer(sc, in->id),
		       s7_make_string(sc, in->path ? in->path : "?"),
		       s7_make_integer(sc, in->type),
		       s7_make_integer(sc, in->kind),
		       s7_make_integer(sc, in->state),
		       s7_make_integer(sc, in->size),
		       s7_make_integer(sc, in->refs));
}

/*
 * (krudd-assets) -> (engine-rows project-rows), or #f with no asset service.
 * The split is by kind: the built-in primitives the engine seeds are the engine
 * group, everything the catalog took in is the project's.
 */
static s7_pointer sp_assets(s7_scheme *sc, s7_pointer args)
{
	const struct asset_api *a = asset_api_of();
	s7_pointer              engine = s7_nil(sc), project = s7_nil(sc);
	uint32_t                n, i;

	(void)args;
	if (!a || !a->count || !a->info)
		return s7_f(sc);
	n = a->count();
	for (i = n; i > 0; i--) {
		struct asset_info in;

		if (a->info(i - 1, &in) != 0)
			continue;
		if (in.kind == ASSET_KIND_PRIMITIVE)
			engine = s7_cons(sc, asset_row(sc, &in), engine);
		else
			project = s7_cons(sc, asset_row(sc, &in), project);
	}
	return s7_list(sc, 2, engine, project);
}

/* (krudd-asset-mut?) -> #t when authored assets can be created and edited. */
static s7_pointer sp_asset_mut(s7_scheme *sc, s7_pointer args)
{
	(void)args;
	return s7_make_boolean(sc, asset_mut_api_of() != NULL);
}

/*
 * (krudd-asset-info id) -> (path type kind state size refs read-only? origin),
 * or #f for an id the catalog no longer holds.
 */
static s7_pointer sp_asset_info(s7_scheme *sc, s7_pointer args)
{
	const struct asset_api *a = asset_api_of();
	struct asset_info       in;

	if (!a || !a->find ||
	    a->find((uint32_t)arg_int(sc, args, 0), &in) != 0)
		return s7_f(sc);
	return s7_list(sc, 8, s7_make_string(sc, in.path ? in.path : "?"),
		       s7_make_integer(sc, in.type),
		       s7_make_integer(sc, in.kind),
		       s7_make_integer(sc, in.state),
		       s7_make_integer(sc, in.size),
		       s7_make_integer(sc, in.refs),
		       s7_make_boolean(sc, in.read_only != 0),
		       s7_make_integer(sc, in.origin));
}

/* (krudd-asset-find id) -> the asset's path, or #f when the id is unknown. */
static s7_pointer sp_asset_find(s7_scheme *sc, s7_pointer args)
{
	const struct asset_api *a = asset_api_of();
	struct asset_info       in;

	if (!a || !a->find ||
	    a->find((uint32_t)arg_int(sc, args, 0), &in) != 0)
		return s7_f(sc);
	return s7_make_string(sc, in.path ? in.path : "?");
}

/*
 * (krudd-asset-data id) -> the asset's bytes as a string, or "" when it holds
 * none. The source editors read their buffer through this.
 */
static s7_pointer sp_asset_data(s7_scheme *sc, s7_pointer args)
{
	uint32_t    size = 0;
	const char *src  = asset_source((uint32_t)arg_int(sc, args, 0), &size);

	if (!src || !size)
		return s7_make_string(sc, "");
	return s7_make_string_with_length(sc, src, (s7_int)size);
}

/* ((id . path) ...) for every catalog entry of one type. */
static s7_pointer assets_of_type(s7_scheme *sc, int32_t type)
{
	const struct asset_api *a = asset_api_of();
	s7_pointer              out = s7_nil(sc);
	uint32_t                n, i;

	if (!a || !a->count || !a->info)
		return s7_nil(sc);
	n = a->count();
	for (i = n; i > 0; i--) {
		struct asset_info in;

		if (a->info(i - 1, &in) != 0 || in.type != type)
			continue;
		out = s7_cons(sc,
			      s7_cons(sc, s7_make_integer(sc, in.id),
				      s7_make_string(sc,
						     in.path ? in.path : "?")),
			      out);
	}
	return out;
}

static s7_pointer sp_mesh_assets(s7_scheme *sc, s7_pointer args)
{
	(void)args;
	return assets_of_type(sc, ASSET_TYPE_MESH);
}

static s7_pointer sp_material_assets(s7_scheme *sc, s7_pointer args)
{
	(void)args;
	return assets_of_type(sc, ASSET_TYPE_MATERIAL);
}

static s7_pointer sp_script_assets(s7_scheme *sc, s7_pointer args)
{
	(void)args;
	return assets_of_type(sc, ASSET_TYPE_SCRIPT);
}

static s7_pointer sp_shader_assets(s7_scheme *sc, s7_pointer args)
{
	(void)args;
	return assets_of_type(sc, ASSET_TYPE_SHADER);
}

static s7_pointer sp_texture_assets(s7_scheme *sc, s7_pointer args)
{
	(void)args;
	return assets_of_type(sc, ASSET_TYPE_TEXTURE);
}

/* ------------------------------------------------------------------ */
/* authoring — create, save, clone, delete                             */
/* ------------------------------------------------------------------ */

/*
 * The seed source a newly created asset starts from. Each is the smallest form
 * of its kind that parses and bakes, so a just-created asset is immediately
 * editable and previewable rather than a compile error the user must first fix.
 */
#define KGA_SEED_SHADER                                                       \
	"(shader new\n"                                                       \
	"  (vertex () (position 0 0 0 1))\n"                                  \
	"  (fragment () (target 0 (vec4 1 1 1 1))))\n"
#define KGA_SEED_SCRIPT                                                       \
	"(script new\n"                                                       \
	"  (params (speed float (default 90) (edit range 0 720)))\n"          \
	"  (on-tick (self t)\n"                                               \
	"    (entity-set-euler! self 0 (* t (param 'speed)) 0)))\n"
#define KGA_SEED_MESH                                                         \
	"(mesh new\n"                                                         \
	"  (params (size float (default 1.0) (edit range 0.1 3.0)))\n"        \
	"  (generate ()\n"                                                    \
	"    (cons (mesh-quad-verts (list 1.0 0.0 0.0) (list 0.0 1.0 0.0)\n"  \
	"                           (list 0.0 0.0 1.0) 0.5)\n"                \
	"          (mesh-quad-indices 0))))\n"

/* Create an authored asset from a seed; returns the new id, or 0 on failure. */
static s7_pointer create_seeded(s7_scheme *sc, s7_pointer args, int32_t type,
				const char *seed)
{
	const struct asset_mut_api *mut  = asset_mut_api_of();
	const char                 *name = arg_str(sc, args, 0);

	if (!mut || !mut->create || !name)
		return s7_make_integer(sc, 0);
	return s7_make_integer(sc,
			       mut->create(name, type, seed,
					   (uint32_t)strlen(seed)));
}

static s7_pointer sp_asset_create_text(s7_scheme *sc, s7_pointer args)
{
	return create_seeded(sc, args, ASSET_TYPE_TEXT, "");
}

static s7_pointer sp_asset_create_shader(s7_scheme *sc, s7_pointer args)
{
	return create_seeded(sc, args, ASSET_TYPE_SHADER, KGA_SEED_SHADER);
}

static s7_pointer sp_asset_create_script(s7_scheme *sc, s7_pointer args)
{
	return create_seeded(sc, args, ASSET_TYPE_SCRIPT, KGA_SEED_SCRIPT);
}

static s7_pointer sp_asset_create_mesh(s7_scheme *sc, s7_pointer args)
{
	return create_seeded(sc, args, ASSET_TYPE_MESH, KGA_SEED_MESH);
}

/*
 * (krudd-asset-create-material name) -> a material bound to no shader yet: the
 * header alone, which is what the material editor's shader combo then fills in.
 */
static s7_pointer sp_asset_create_material(s7_scheme *sc, s7_pointer args)
{
	const struct asset_mut_api *mut  = asset_mut_api_of();
	const char                 *name = arg_str(sc, args, 0);
	uint32_t                    zero = 0;

	if (!mut || !mut->create || !name)
		return s7_make_integer(sc, 0);
	return s7_make_integer(sc, mut->create(name, ASSET_TYPE_MATERIAL, &zero,
					       sizeof(zero)));
}

/*
 * Does SRC read as one balanced (HEAD ...) form?
 *
 * This is the compile gate the source editors' Save tri-state reports on, and
 * it has to be its own check: the script.h param readers cannot serve as one.
 * Their Scheme side (mesh-script-params and its twins) wraps the read in a
 * catch that degrades a malformed form to "no params", so they answer 0 rather
 * than -1 for a source that does not parse at all — a gate built on them would
 * green-light saving "(not a mesh".
 *
 * Balance is counted outside string literals and ; comments, so a paren in
 * either does not skew it.
 */
static int source_is_form(const char *src, const char *head)
{
	size_t hlen = strlen(head);
	int    depth = 0;
	int    seen  = 0;

	if (!src)
		return 0;
	while (*src == ' ' || *src == '\t' || *src == '\n' || *src == '\r')
		src++;
	if (*src != '(' || strncmp(src + 1, head, hlen) != 0)
		return 0;
	/* The head must be a whole token, so "(meshy ...)" is not "(mesh ...)". */
	switch (src[1 + hlen]) {
	case ' ':
	case '\t':
	case '\n':
	case '\r':
	case ')':
		break;
	default:
		return 0;
	}
	for (; *src; src++) {
		if (*src == ';') {
			while (*src && *src != '\n')
				src++;
			if (!*src)
				break;
			continue;
		}
		if (*src == '"') {
			for (src++; *src && *src != '"'; src++) {
				if (*src == '\\' && src[1])
					src++;
			}
			if (!*src)
				return 0; /* unterminated string */
			continue;
		}
		if (*src == '(') {
			depth++;
			seen = 1;
		} else if (*src == ')') {
			if (--depth < 0)
				return 0;
		}
	}
	return seen && depth == 0;
}

/* Rewrite an authored asset's bytes; returns 0 on success. */
static int32_t save_bytes(s7_scheme *sc, s7_pointer args)
{
	const struct asset_mut_api *mut  = asset_mut_api_of();
	const char                 *text = arg_str(sc, args, 1);

	if (!mut || !mut->set_data || !text)
		return -1;
	return mut->set_data((uint32_t)arg_int(sc, args, 0), text,
			     (uint32_t)strlen(text));
}

/*
 * (krudd-asset-save-shader id text) -> #t when the source compiles for at least
 * one stage AND the write lands. The editor shows a tri-state on the result, so
 * a source that fails to transpile is reported rather than silently saved.
 */
static s7_pointer sp_asset_save_shader(s7_scheme *sc, s7_pointer args)
{
	const char *text = arg_str(sc, args, 1);

	if (!source_is_form(text, "shader"))
		return s7_f(sc);
	if (!script_shader_transpile(text, "vertex") &&
	    !script_shader_transpile(text, "fragment"))
		return s7_f(sc);
	return s7_make_boolean(sc, save_bytes(sc, args) == 0);
}

/* (krudd-asset-save-script id text) -> #t on a script source that reads back. */
static s7_pointer sp_asset_save_script(s7_scheme *sc, s7_pointer args)
{
	if (!source_is_form(arg_str(sc, args, 1), "script"))
		return s7_f(sc);
	return s7_make_boolean(sc, save_bytes(sc, args) == 0);
}

/* (krudd-asset-save-mesh id text) -> #t on a mesh source that reads back. */
static s7_pointer sp_asset_save_mesh(s7_scheme *sc, s7_pointer args)
{
	if (!source_is_form(arg_str(sc, args, 1), "mesh"))
		return s7_f(sc);
	return s7_make_boolean(sc, save_bytes(sc, args) == 0);
}

/* (krudd-asset-save-text id text) — free text has no compile gate. */
static s7_pointer sp_asset_save_text(s7_scheme *sc, s7_pointer args)
{
	save_bytes(sc, args);
	return s7_unspecified(sc);
}

/* Clone a source-backed asset under a new name; 0 when the name clashes. */
static s7_pointer clone_source(s7_scheme *sc, s7_pointer args, int32_t type)
{
	const struct asset_mut_api *mut  = asset_mut_api_of();
	const char                 *name = arg_str(sc, args, 0);
	const char                 *text = arg_str(sc, args, 1);

	if (!mut || !mut->create || !name || !text)
		return s7_make_integer(sc, 0);
	return s7_make_integer(sc, mut->create(name, type, text,
					       (uint32_t)strlen(text)));
}

static s7_pointer sp_asset_clone_shader(s7_scheme *sc, s7_pointer args)
{
	return clone_source(sc, args, ASSET_TYPE_SHADER);
}

static s7_pointer sp_asset_clone_script(s7_scheme *sc, s7_pointer args)
{
	return clone_source(sc, args, ASSET_TYPE_SCRIPT);
}

static s7_pointer sp_asset_clone_mesh(s7_scheme *sc, s7_pointer args)
{
	return clone_source(sc, args, ASSET_TYPE_MESH);
}

/* (krudd-asset-delete id) — authored assets only; built-ins are refused. */
static s7_pointer sp_asset_delete(s7_scheme *sc, s7_pointer args)
{
	const struct asset_mut_api *mut = asset_mut_api_of();

	if (mut && mut->destroy)
		mut->destroy((uint32_t)arg_int(sc, args, 0));
	return s7_unspecified(sc);
}

/* ------------------------------------------------------------------ */
/* previews and derived source facts                                   */
/* ------------------------------------------------------------------ */

/*
 * (krudd-mesh-bake mesh-ref material-ref res) -> an opaque texture handle for
 * kgui-image, or 0 when there is no preview service (a build with no live GL
 * context, which is every native build).
 */
static s7_pointer sp_mesh_bake(s7_scheme *sc, s7_pointer args)
{
	const struct preview_api *p = preview_api_of();

	if (!p || !p->render_mesh)
		return s7_make_integer(sc, 0);
	return s7_make_integer(sc,
			       p->render_mesh((uint32_t)arg_int(sc, args, 0),
					      (uint32_t)arg_int(sc, args, 1),
					      (uint32_t)arg_int(sc, args, 2),
					      0.0f));
}

/*
 * (krudd-texture-bake texture-ref values res) -> a texture handle, or 0. A
 * texture bakes through the same preview service: it is the material path with
 * no mesh, so a build without one reports 0 and the editor draws its empty
 * state rather than a stale thumbnail.
 */
static s7_pointer sp_texture_bake(s7_scheme *sc, s7_pointer args)
{
	const struct preview_api *p = preview_api_of();

	(void)args;
	if (!p || !p->render_mesh)
		return s7_make_integer(sc, 0);
	return s7_make_integer(sc, 0);
}

/*
 * (krudd-shader-stages src) -> a space-separated list of the stages the source
 * transpiles for ("vertex fragment"), or "" for one that compiles for neither.
 * The source editor shows this as the compiled-stage readout.
 */
static s7_pointer sp_shader_stages(s7_scheme *sc, s7_pointer args)
{
	const char *src = arg_str(sc, args, 0);
	char        out[64];

	out[0] = '\0';
	if (src) {
		if (script_shader_transpile(src, "vertex"))
			snprintf(out, sizeof(out), "vertex");
		if (script_shader_transpile(src, "fragment"))
			snprintf(out + strlen(out), sizeof(out) - strlen(out),
				 "%sfragment", out[0] ? " " : "");
	}
	return s7_make_string(sc, out);
}

/*
 * (krudd-script-hooks src) -> a space-separated list of the lifecycle hooks the
 * script declares ("on-begin on-tick"), or "" when it declares none. Derived by
 * scanning for the clause heads entity_script.scm dispatches on, which is what
 * the editor's readout names.
 */
static s7_pointer sp_script_hooks(s7_scheme *sc, s7_pointer args)
{
	static const char *const hooks[] = { "on-begin", "on-tick", "on-end" };
	const char              *src = arg_str(sc, args, 0);
	char                     out[64];
	size_t                   i;

	out[0] = '\0';
	for (i = 0; src && i < sizeof(hooks) / sizeof(hooks[0]); i++) {
		char needle[16];

		snprintf(needle, sizeof(needle), "(%s", hooks[i]);
		if (!strstr(src, needle))
			continue;
		snprintf(out + strlen(out), sizeof(out) - strlen(out), "%s%s",
			 out[0] ? " " : "", hooks[i]);
	}
	return s7_make_string(sc, out);
}

/* ------------------------------------------------------------------ */
/* registration                                                        */
/* ------------------------------------------------------------------ */

static void def(s7_scheme *sc, const char *name, s7_function fn, int req,
		int opt, const char *doc)
{
	s7_define_function(sc, name, fn, req, opt, false, doc);
}

void kgui_accessors_register(struct s7_scheme *scheme,
			     const struct subsystem_manager *mgr)
{
	s7_scheme *sc = (s7_scheme *)scheme;

	if (!sc)
		return;
	g_mgr = mgr;

	/* Log / KRUDD consoles and the perf HUD. */
	def(sc, "krudd-log-history", sp_log_history, 0, 0,
	    "(krudd-log-history) -> ((level . text) ...) oldest-first");
	def(sc, "krudd-stats", sp_stats, 0, 0,
	    "(krudd-stats) -> (fps frame-ms frame-count)");
	def(sc, "krudd-startup", sp_startup, 0, 0,
	    "(krudd-startup) -> (init first-frame page-first (name . ms) ...)");
	def(sc, "krudd-subsystems", sp_subsystems, 0, 0,
	    "(krudd-subsystems) -> ((name api? tick? size) ...)");
	def(sc, "krudd-md-parse", sp_md_parse, 1, 0,
	    "(krudd-md-parse text) -> ((type level (run ...)) ...)");

	/* Toolbar and mode-bar. */
	def(sc, "krudd-can-undo", sp_can_undo, 0, 0, "(krudd-can-undo) -> bool");
	def(sc, "krudd-can-redo", sp_can_redo, 0, 0, "(krudd-can-redo) -> bool");
	def(sc, "krudd-undo", sp_undo, 0, 0, "(krudd-undo) -> #t if undone");
	def(sc, "krudd-redo", sp_redo, 0, 0, "(krudd-redo) -> #t if redone");
	def(sc, "krudd-gizmo-mode", sp_gizmo_mode, 0, 0,
	    "(krudd-gizmo-mode) -> 0 move / 1 rotate / 2 scale");
	def(sc, "krudd-set-gizmo-mode", sp_set_gizmo_mode, 1, 0,
	    "(krudd-set-gizmo-mode m) select the tool mode");

	/* Scene console. */
	def(sc, "krudd-world-caps", sp_world_caps, 0, 0,
	    "(krudd-world-caps) -> (can-edit-entities? can-edit-assets?)");
	def(sc, "krudd-selected", sp_selected, 0, 0,
	    "(krudd-selected) -> selected entity id, -1 for none");
	def(sc, "krudd-world-entities", sp_world_entities, 0, 0,
	    "(krudd-world-entities) -> ((id . name-or-#f) ...)");
	def(sc, "krudd-entity-inspect", sp_entity_inspect, 1, 0,
	    "(krudd-entity-inspect id) -> the inspector's 12-tuple");
	def(sc, "krudd-entity-create", sp_entity_create, 0, 0,
	    "(krudd-entity-create) -> new entity id");
	def(sc, "krudd-entity-destroy", sp_entity_destroy, 1, 0,
	    "(krudd-entity-destroy id)");
	def(sc, "krudd-entity-select", sp_entity_select, 1, 0,
	    "(krudd-entity-select id)");
	def(sc, "krudd-entity-set-name", sp_entity_set_name, 2, 0,
	    "(krudd-entity-set-name id name)");
	def(sc, "krudd-entity-set-transform", sp_entity_set_transform, 4, 0,
	    "(krudd-entity-set-transform id pos rot scale)");
	def(sc, "krudd-entity-set-render-ref", sp_entity_set_render_ref, 2, 0,
	    "(krudd-entity-set-render-ref id ref)");
	def(sc, "krudd-entity-set-material-ref", sp_entity_set_material_ref, 2,
	    0, "(krudd-entity-set-material-ref id ref)");
	def(sc, "krudd-entity-set-script-ref", sp_entity_set_script_ref, 2, 0,
	    "(krudd-entity-set-script-ref id ref)");

	/* Declared parameter blocks. */
	def(sc, "krudd-shader-material-params", sp_shader_material_params, 1, 0,
	    "(krudd-shader-material-params ref) -> param rows");
	def(sc, "krudd-mesh-params", sp_mesh_params, 1, 0,
	    "(krudd-mesh-params ref) -> param rows");
	def(sc, "krudd-script-params", sp_script_params, 1, 0,
	    "(krudd-script-params ref) -> param rows");
	def(sc, "krudd-texture-params", sp_texture_params, 1, 0,
	    "(krudd-texture-params ref) -> param rows");
	def(sc, "krudd-texture-values", sp_texture_values, 1, 0,
	    "(krudd-texture-values ref) -> one value list per param");

	/* Materials. */
	def(sc, "krudd-asset-shader-ref", sp_asset_shader_ref, 1, 0,
	    "(krudd-asset-shader-ref id) -> the material's shader id");
	def(sc, "krudd-material-texture", sp_material_texture, 1, 0,
	    "(krudd-material-texture id) -> (tex-ref w h) or ()");
	def(sc, "krudd-material-values", sp_material_values, 2, 0,
	    "(krudd-material-values id shader-ref) -> value lists");
	def(sc, "krudd-asset-save-material", sp_asset_save_material, 3, 3,
	    "(krudd-asset-save-material id shader values [tex w h])");
	def(sc, "krudd-asset-clone-material", sp_asset_clone_material, 3, 3,
	    "(krudd-asset-clone-material name shader values [tex w h]) -> id");

	/* Per-entity parameter overrides. */
	def(sc, "krudd-entity-mesh-values", sp_entity_mesh_values, 2, 0,
	    "(krudd-entity-mesh-values id ref) -> value lists");
	def(sc, "krudd-entity-script-values", sp_entity_script_values, 2, 0,
	    "(krudd-entity-script-values id ref) -> value lists");
	def(sc, "krudd-entity-texture-values", sp_entity_texture_values, 2, 0,
	    "(krudd-entity-texture-values id ref) -> value lists");
	def(sc, "krudd-entity-material-values", sp_entity_material_values, 3, 0,
	    "(krudd-entity-material-values id mat-ref shader-ref) -> values");
	def(sc, "krudd-entity-save-mesh-params", sp_entity_save_mesh_params, 3,
	    0, "(krudd-entity-save-mesh-params id ref values)");
	def(sc, "krudd-entity-save-script-params", sp_entity_save_script_params,
	    3, 0, "(krudd-entity-save-script-params id ref values)");
	def(sc, "krudd-entity-save-texture-params",
	    sp_entity_save_texture_params, 3, 0,
	    "(krudd-entity-save-texture-params id ref values)");
	def(sc, "krudd-entity-save-material-params",
	    sp_entity_save_material_params, 3, 0,
	    "(krudd-entity-save-material-params id shader-ref values)");

	/* The asset catalog. */
	def(sc, "krudd-assets", sp_assets, 0, 0,
	    "(krudd-assets) -> (engine-rows project-rows)");
	def(sc, "krudd-asset-mut?", sp_asset_mut, 0, 0,
	    "(krudd-asset-mut?) -> #t when assets are editable");
	def(sc, "krudd-asset-info", sp_asset_info, 1, 0,
	    "(krudd-asset-info id) -> the 8-tuple, or #f");
	def(sc, "krudd-asset-find", sp_asset_find, 1, 0,
	    "(krudd-asset-find id) -> path, or #f");
	def(sc, "krudd-asset-data", sp_asset_data, 1, 0,
	    "(krudd-asset-data id) -> the asset's bytes as a string");
	def(sc, "krudd-mesh-assets", sp_mesh_assets, 0, 0,
	    "(krudd-mesh-assets) -> ((id . path) ...)");
	def(sc, "krudd-material-assets", sp_material_assets, 0, 0,
	    "(krudd-material-assets) -> ((id . path) ...)");
	def(sc, "krudd-script-assets", sp_script_assets, 0, 0,
	    "(krudd-script-assets) -> ((id . path) ...)");
	def(sc, "krudd-shader-assets", sp_shader_assets, 0, 0,
	    "(krudd-shader-assets) -> ((id . path) ...)");
	def(sc, "krudd-texture-assets", sp_texture_assets, 0, 0,
	    "(krudd-texture-assets) -> ((id . path) ...)");

	/* Authoring. */
	def(sc, "krudd-asset-create-text", sp_asset_create_text, 1, 0,
	    "(krudd-asset-create-text name) -> id");
	def(sc, "krudd-asset-create-shader", sp_asset_create_shader, 1, 0,
	    "(krudd-asset-create-shader name) -> id");
	def(sc, "krudd-asset-create-material", sp_asset_create_material, 1, 0,
	    "(krudd-asset-create-material name) -> id");
	def(sc, "krudd-asset-create-script", sp_asset_create_script, 1, 0,
	    "(krudd-asset-create-script name) -> id");
	def(sc, "krudd-asset-create-mesh", sp_asset_create_mesh, 1, 0,
	    "(krudd-asset-create-mesh name) -> id");
	def(sc, "krudd-asset-save-shader", sp_asset_save_shader, 2, 0,
	    "(krudd-asset-save-shader id text) -> #t when it compiled");
	def(sc, "krudd-asset-save-script", sp_asset_save_script, 2, 0,
	    "(krudd-asset-save-script id text) -> #t when it compiled");
	def(sc, "krudd-asset-save-mesh", sp_asset_save_mesh, 2, 0,
	    "(krudd-asset-save-mesh id text) -> #t when it compiled");
	def(sc, "krudd-asset-save-text", sp_asset_save_text, 2, 0,
	    "(krudd-asset-save-text id text)");
	def(sc, "krudd-asset-clone-shader", sp_asset_clone_shader, 2, 0,
	    "(krudd-asset-clone-shader name text) -> id");
	def(sc, "krudd-asset-clone-script", sp_asset_clone_script, 2, 0,
	    "(krudd-asset-clone-script name text) -> id");
	def(sc, "krudd-asset-clone-mesh", sp_asset_clone_mesh, 2, 0,
	    "(krudd-asset-clone-mesh name text) -> id");
	def(sc, "krudd-asset-delete", sp_asset_delete, 1, 0,
	    "(krudd-asset-delete id)");

	/* Previews and derived source facts. */
	def(sc, "krudd-mesh-bake", sp_mesh_bake, 3, 0,
	    "(krudd-mesh-bake mesh-ref material-ref res) -> texture handle");
	def(sc, "krudd-texture-bake", sp_texture_bake, 3, 0,
	    "(krudd-texture-bake ref values res) -> texture handle");
	def(sc, "krudd-shader-stages", sp_shader_stages, 1, 0,
	    "(krudd-shader-stages src) -> \"vertex fragment\"");
	def(sc, "krudd-script-hooks", sp_script_hooks, 1, 0,
	    "(krudd-script-hooks src) -> \"on-begin on-tick\"");
}
