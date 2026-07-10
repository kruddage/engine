/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Native test for the Scheme-authored KRUDD-tab panels (kruddboard.scm).
 *
 * The real kruddboard primitives bind s7 to Dear ImGui, which only builds for
 * the browser (WASM) target — so the panel logic could not be exercised off a
 * device.  This harness closes that gap: it registers *stub* primitives that
 * record their calls instead of drawing, loads the same image the WASM host
 * loads (KRUDDBOARD_SCM), runs each panel procedure, and asserts on the
 * recorded draw-call sequence.  It verifies the portable logic — table layout,
 * the log level filter, the level→colour map, the KRUDD composition — leaving
 * only pixel-level ImGui layout to browser verification.
 */
#include "script.h"

#include "s7.h"
#include "kruddboard_scm.h"
#include "assets_scm.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int tests_run;
static int tests_passed;

#define RUN(name) do { \
	tests_run++; \
	test_##name(); \
	tests_passed++; \
	printf("PASS: " #name "\n"); \
} while (0)

/* ------------------------------------------------------------------ */
/* Recorded draw calls                                                 */
/* ------------------------------------------------------------------ */

#define REC_MAX 512
#define REC_LEN 192

static char g_rec[REC_MAX][REC_LEN];
static int  g_rec_n;

static void rec_reset(void)
{
	g_rec_n = 0;
}

static void rec(const char *fmt, ...)
{
	va_list ap;

	if (g_rec_n >= REC_MAX)
		return;
	va_start(ap, fmt);
	vsnprintf(g_rec[g_rec_n], REC_LEN, fmt, ap);
	va_end(ap);
	g_rec_n++;
}

static int rec_count(const char *needle)
{
	int c = 0;
	int i;

	for (i = 0; i < g_rec_n; i++)
		if (strstr(g_rec[i], needle))
			c++;
	return c;
}

static int rec_has(const char *needle)
{
	return rec_count(needle) > 0;
}

/* Index of the first recorded line containing NEEDLE, or -1. */
static int rec_index(const char *needle)
{
	int i;

	for (i = 0; i < g_rec_n; i++)
		if (strstr(g_rec[i], needle))
			return i;
	return -1;
}

/* ------------------------------------------------------------------ */
/* Simulated frame state                                               */
/* ------------------------------------------------------------------ */

/* Label of the button/checkbox "clicked" this frame (NULL = nothing). */
static const char *g_click;
static int         g_subsys_absent;
static int         g_log_absent;

static int clicked(const char *label)
{
	return g_click && strcmp(g_click, label) == 0;
}

/* imgui-begin/end-disabled nesting; a widget is inert while any level is set. */
static int g_dis[16];
static int g_dis_top;

static int disabled_now(void)
{
	int i;

	for (i = 0; i < g_dis_top; i++)
		if (g_dis[i])
			return 1;
	return 0;
}

/* ------------------------------------------------------------------ */
/* Stub primitives                                                     */
/* ------------------------------------------------------------------ */

static s7_pointer st_text(s7_scheme *sc, s7_pointer a)
{
	rec("text|%s", s7_string(s7_car(a)));
	return s7_unspecified(sc);
}

static s7_pointer st_text_disabled(s7_scheme *sc, s7_pointer a)
{
	rec("disabled|%s", s7_string(s7_car(a)));
	return s7_unspecified(sc);
}

static s7_pointer st_text_colored(s7_scheme *sc, s7_pointer a)
{
	s7_pointer  p = a;
	double      r = s7_number_to_real(sc, s7_car(p)); p = s7_cdr(p);
	double      g = s7_number_to_real(sc, s7_car(p)); p = s7_cdr(p);
	double      b = s7_number_to_real(sc, s7_car(p)); p = s7_cdr(p);
	double      al = s7_number_to_real(sc, s7_car(p)); p = s7_cdr(p);
	const char *s = s7_string(s7_car(p));

	rec("colored|%.2f,%.2f,%.2f,%.2f|%s", r, g, b, al, s);
	return s7_unspecified(sc);
}

static s7_pointer st_small_button(s7_scheme *sc, s7_pointer a)
{
	const char *l = s7_string(s7_car(a));

	rec("button|%s", l);
	return s7_make_boolean(sc, clicked(l) && !disabled_now());
}

static s7_pointer st_same_line(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	rec("sameline");
	return s7_unspecified(sc);
}

static s7_pointer st_checkbox(s7_scheme *sc, s7_pointer a)
{
	const char *l     = s7_string(s7_car(a));
	int         state = s7_boolean(sc, s7_cadr(a));

	rec("checkbox|%s|%d", l, state);
	if (clicked(l))
		state = !state;
	return s7_make_boolean(sc, state);
}

static s7_pointer st_separator(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	rec("sep");
	return s7_unspecified(sc);
}

static s7_pointer st_collapsing_header(s7_scheme *sc, s7_pointer a)
{
	s7_pointer rest         = s7_cdr(a);
	int        default_open = s7_is_pair(rest) ?
				   s7_boolean(sc, s7_car(rest)) : 1;

	rec("header|%s|%d", s7_string(s7_car(a)), default_open);
	return s7_make_boolean(sc, 1);
}

static s7_pointer st_begin_table(s7_scheme *sc, s7_pointer a)
{
	rec("table-begin|%s|%lld", s7_string(s7_car(a)),
	    (long long)s7_integer(s7_cadr(a)));
	return s7_make_boolean(sc, 1);
}

static s7_pointer st_setup_column(s7_scheme *sc, s7_pointer a)
{
	rec("col|%s", s7_string(s7_car(a)));
	return s7_unspecified(sc);
}

static s7_pointer st_headers_row(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	rec("headers");
	return s7_unspecified(sc);
}

static s7_pointer st_next_row(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	rec("row");
	return s7_unspecified(sc);
}

static s7_pointer st_next_column(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	rec("cell");
	return s7_make_boolean(sc, 1);
}

static s7_pointer st_end_table(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	rec("table-end");
	return s7_unspecified(sc);
}

static s7_pointer st_begin_child(s7_scheme *sc, s7_pointer a)
{
	rec("child-begin|%s", s7_string(s7_car(a)));
	return s7_unspecified(sc);
}

static s7_pointer st_end_child(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	rec("child-end");
	return s7_unspecified(sc);
}

static s7_pointer st_scroll_here(s7_scheme *sc, s7_pointer a)
{
	rec("scrollhere|%.2f", s7_number_to_real(sc, s7_car(a)));
	return s7_unspecified(sc);
}

static s7_pointer st_work_height(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	return s7_make_real(sc, 1000.0);
}

static s7_pointer st_stats(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	return s7_list(sc, 3, s7_make_real(sc, 60.0),
		       s7_make_real(sc, 16.5), s7_make_integer(sc, 42));
}

/*
 * Fixture: three subsystems exercising every cell shape —
 *   log        api=yes tick=yes size=1024
 *   memory     api=yes tick=-   size=- (zero)
 *   kruddboard api=-   tick=yes size=- (zero)
 */
static s7_pointer st_subsystems(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	if (g_subsys_absent)
		return s7_f(sc);
	return s7_list(sc, 3,
		s7_list(sc, 4, s7_make_string(sc, "log"),
			s7_make_boolean(sc, 1), s7_make_boolean(sc, 1),
			s7_make_integer(sc, 1024)),
		s7_list(sc, 4, s7_make_string(sc, "memory"),
			s7_make_boolean(sc, 1), s7_make_boolean(sc, 0),
			s7_make_integer(sc, 0)),
		s7_list(sc, 4, s7_make_string(sc, "kruddboard"),
			s7_make_boolean(sc, 0), s7_make_boolean(sc, 1),
			s7_make_integer(sc, 0)));
}

/* Fixture: one history line per level, oldest-first (DEBUG..ERROR). */
static s7_pointer st_log_history(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	if (g_log_absent)
		return s7_f(sc);
	return s7_list(sc, 4,
		s7_cons(sc, s7_make_integer(sc, 0), s7_make_string(sc, "dbg line")),
		s7_cons(sc, s7_make_integer(sc, 1), s7_make_string(sc, "info line")),
		s7_cons(sc, s7_make_integer(sc, 2), s7_make_string(sc, "warn line")),
		s7_cons(sc, s7_make_integer(sc, 3), s7_make_string(sc, "err line")));
}

/* ------------------------------------------------------------------ */
/* World-tab stub primitives + fixture (#401)                          */
/* ------------------------------------------------------------------ */

/* Simulated per-frame input for the two-way widgets. */
static const char *g_input_id;      /* id whose input-text is edited      */
static const char *g_input_text;    /* text it now holds                  */
static int         g_input_commit;  /* did it just commit (focus lost)?   */
static const char *g_float_id;      /* id whose input-floatN changed      */
static int         g_float_changed;
static const char *g_combo_open;    /* id of the one open combo, or NULL  */

static s7_pointer st_begin_disabled(s7_scheme *sc, s7_pointer a)
{
	int f = s7_boolean(sc, s7_car(a));

	if (g_dis_top < 16)
		g_dis[g_dis_top++] = f;
	rec("dis-begin|%d", f);
	return s7_unspecified(sc);
}

static s7_pointer st_end_disabled(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	if (g_dis_top > 0)
		g_dis_top--;
	rec("dis-end");
	return s7_unspecified(sc);
}

static s7_pointer st_set_next_item_width(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	return s7_unspecified(sc);
}

static s7_pointer st_input_text(s7_scheme *sc, s7_pointer a)
{
	const char *id  = s7_string(s7_car(a));
	const char *cur = s7_is_string(s7_cadr(a)) ? s7_string(s7_cadr(a)) : "";
	int         done = 0;

	if (g_input_id && strcmp(g_input_id, id) == 0) {
		if (g_input_text)
			cur = g_input_text;
		done = g_input_commit;
	}
	rec("input-text|%s|%s", id, cur);
	return s7_cons(sc, s7_make_string(sc, cur), s7_make_boolean(sc, done));
}

static s7_pointer st_input_float(s7_scheme *sc, s7_pointer a, const char *tag)
{
	const char *id      = s7_string(s7_car(a));
	int         changed = 0;

	if (g_float_id && strcmp(g_float_id, id) == 0)
		changed = g_float_changed;
	rec("%s|%s", tag, id);
	return s7_cons(sc, s7_cadr(a), s7_make_boolean(sc, changed));
}

static s7_pointer st_input_float3(s7_scheme *sc, s7_pointer a)
{
	return st_input_float(sc, a, "input-f3");
}

static s7_pointer st_input_float4(s7_scheme *sc, s7_pointer a)
{
	return st_input_float(sc, a, "input-f4");
}

static s7_pointer st_begin_combo(s7_scheme *sc, s7_pointer a)
{
	const char *id      = s7_string(s7_car(a));
	const char *preview = s7_string(s7_cadr(a));
	int         open    = g_combo_open && strcmp(g_combo_open, id) == 0;

	rec("combo|%s|%s", id, preview);
	return s7_make_boolean(sc, open);
}

static s7_pointer st_end_combo(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	rec("combo-end");
	return s7_unspecified(sc);
}

static s7_pointer st_selectable(s7_scheme *sc, s7_pointer a)
{
	const char *l   = s7_string(s7_car(a));
	int         sel = s7_boolean(sc, s7_cadr(a));

	rec("selectable|%s|%d", l, sel);
	return s7_make_boolean(sc, clicked(l) && !disabled_now());
}

static s7_pointer st_default_focus(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	rec("default-focus");
	return s7_unspecified(sc);
}

static s7_pointer st_calc_text_width(s7_scheme *sc, s7_pointer a)
{
	return s7_make_real(sc, (double)(strlen(s7_string(s7_car(a))) * 7));
}

static s7_pointer st_same_line_right(s7_scheme *sc, s7_pointer a)
{
	rec("sameline-right|%.1f", s7_number_to_real(sc, s7_car(a)));
	return s7_unspecified(sc);
}

static s7_pointer st_push_btn_color(s7_scheme *sc, s7_pointer a)
{
	s7_pointer p = a;
	double     r = s7_number_to_real(sc, s7_car(p)); p = s7_cdr(p);
	double     g = s7_number_to_real(sc, s7_car(p)); p = s7_cdr(p);
	double     b = s7_number_to_real(sc, s7_car(p));

	rec("push-btn-color|%.2f,%.2f,%.2f", r, g, b);
	return s7_unspecified(sc);
}

static s7_pointer st_pop_color(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	rec("pop-color");
	return s7_unspecified(sc);
}

static s7_pointer st_begin_table_plain(s7_scheme *sc, s7_pointer a)
{
	rec("table-plain|%s|%lld", s7_string(s7_car(a)),
	    (long long)s7_integer(s7_cadr(a)));
	return s7_make_boolean(sc, 1);
}

static s7_pointer st_setup_column_fixed(s7_scheme *sc, s7_pointer a)
{
	rec("col-fixed|%s|%.0f", s7_string(s7_car(a)),
	    s7_number_to_real(sc, s7_cadr(a)));
	return s7_unspecified(sc);
}

/* Fake entity world driven by the mutating primitives. */
#define FW_MAX 8

struct fw_entity {
	int      alive;
	int      has_name;
	char     name[64];
	float    pos[3], rot[4], scl[3];
	int      parent;
	int      has_render, has_material, has_script;
	unsigned render_ref, material_ref, script_ref;
};

static struct fw_entity g_fw[FW_MAX];
static int              g_fw_count;
static int              g_fw_sel;
static int              g_have_entity;
static int              g_have_asset;
static int              g_gizmo;

/* type: 1 = ASSET_TYPE_MESH, 3 = ASSET_TYPE_MATERIAL, 8 = ASSET_TYPE_SCRIPT
 * (per asset_api.h).
 */
struct fw_asset {
	unsigned    id;
	const char *path;
	int         type;
};

static struct fw_asset g_assets[8];
static int             g_assets_n;

static void fw_reset(void)
{
	int i;

	memset(g_fw, 0, sizeof(g_fw));
	for (i = 0; i < FW_MAX; i++)
		g_fw[i].parent = -1;

	/* id0: "Cube", root, bound to mesh 101, no material. */
	g_fw[0].alive = 1;
	g_fw[0].has_name = 1;
	strcpy(g_fw[0].name, "Cube");
	g_fw[0].rot[3] = 1.0f;
	g_fw[0].scl[0] = g_fw[0].scl[1] = g_fw[0].scl[2] = 1.0f;
	g_fw[0].has_render = 1;
	g_fw[0].render_ref = 101;

	/* id1: unnamed, root, no bindings — exercises the "entity N" fallback. */
	g_fw[1].alive = 1;
	g_fw[1].rot[3] = 1.0f;
	g_fw[1].scl[0] = g_fw[1].scl[1] = g_fw[1].scl[2] = 1.0f;

	g_fw_count    = 2;
	g_fw_sel      = 0;
	g_have_entity = 1;
	g_have_asset  = 1;
	g_gizmo       = 0;

	g_assets[0].id = 101; g_assets[0].path = "cube.mesh";   g_assets[0].type = 1;
	g_assets[1].id = 102; g_assets[1].path = "sphere.mesh"; g_assets[1].type = 1;
	g_assets[2].id = 201; g_assets[2].path = "red.mat";     g_assets[2].type = 3;
	g_assets[3].id = 301; g_assets[3].path = "orbit.kscm";  g_assets[3].type = 8;
	g_assets_n = 4;

	g_click        = NULL;
	g_dis_top      = 0;
	g_input_id     = NULL;
	g_input_text   = NULL;
	g_input_commit = 0;
	g_float_id     = NULL;
	g_float_changed = 0;
	g_combo_open   = NULL;
}

static s7_pointer real_vec(s7_scheme *sc, const float *v, int n)
{
	s7_pointer out = s7_nil(sc);
	int        i;

	for (i = n - 1; i >= 0; i--)
		out = s7_cons(sc, s7_make_real(sc, v[i]), out);
	return out;
}

static s7_pointer st_world_caps(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	return s7_list(sc, 2, s7_make_boolean(sc, g_have_entity),
		       s7_make_boolean(sc, g_have_asset));
}

static s7_pointer st_selected(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	return s7_make_integer(sc, g_fw_sel);
}

static s7_pointer st_world_entities(s7_scheme *sc, s7_pointer a)
{
	s7_pointer out;
	int        i;

	(void)a;
	if (!g_have_entity)
		return s7_f(sc);
	out = s7_nil(sc);
	for (i = 0; i < g_fw_count; i++) {
		s7_pointer name;

		if (!g_fw[i].alive)
			continue;
		name = g_fw[i].has_name ? s7_make_string(sc, g_fw[i].name)
					: s7_f(sc);
		out = s7_cons(sc, s7_cons(sc, s7_make_integer(sc, i), name),
			      out);
	}
	return s7_reverse(sc, out);
}

static s7_pointer st_entity_inspect(s7_scheme *sc, s7_pointer a)
{
	int               id = (int)s7_integer(s7_car(a));
	struct fw_entity *e;
	s7_pointer        parent;

	if (!g_have_entity || id < 0 || id >= g_fw_count || !g_fw[id].alive)
		return s7_f(sc);
	e = &g_fw[id];
	if (e->parent < 0)
		parent = s7_f(sc);
	else
		parent = s7_cons(sc, s7_make_integer(sc, e->parent),
				 g_fw[e->parent].has_name
				 ? s7_make_string(sc, g_fw[e->parent].name)
				 : s7_f(sc));
	return s7_list(sc, 12,
		s7_make_string(sc, e->name),
		real_vec(sc, e->pos, 3),
		real_vec(sc, e->rot, 4),
		real_vec(sc, e->scl, 3),
		parent,
		s7_make_boolean(sc, e->has_name),
		s7_make_boolean(sc, e->has_render),
		s7_make_boolean(sc, e->has_material),
		s7_make_integer(sc, e->has_render ? (int)e->render_ref : 0),
		s7_make_integer(sc, e->has_material ? (int)e->material_ref : 0),
		s7_make_boolean(sc, e->has_script),
		s7_make_integer(sc, e->has_script ? (int)e->script_ref : 0));
}

static s7_pointer assets_by_type(s7_scheme *sc, int type)
{
	s7_pointer out = s7_nil(sc);
	int        i;

	if (!g_have_asset)
		return out;
	for (i = g_assets_n - 1; i >= 0; i--)
		if (g_assets[i].type == type)
			out = s7_cons(sc, s7_cons(sc,
				s7_make_integer(sc, g_assets[i].id),
				s7_make_string(sc, g_assets[i].path)), out);
	return out;
}

static s7_pointer st_mesh_assets(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	return assets_by_type(sc, 1);
}

static s7_pointer st_material_assets(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	return assets_by_type(sc, 3);
}

static s7_pointer st_script_assets(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	return assets_by_type(sc, 8);
}

static s7_pointer st_asset_find(s7_scheme *sc, s7_pointer a)
{
	unsigned ref = (unsigned)s7_integer(s7_car(a));
	int      i;

	if (ref == 0)
		return s7_f(sc);
	for (i = 0; i < g_assets_n; i++)
		if (g_assets[i].id == ref)
			return s7_make_string(sc, g_assets[i].path);
	return s7_f(sc);
}

static s7_pointer st_entity_create(s7_scheme *sc, s7_pointer a)
{
	int id;

	(void)a;
	if (!g_have_entity || g_fw_count >= FW_MAX)
		return s7_make_integer(sc, -1);
	id = g_fw_count++;
	memset(&g_fw[id], 0, sizeof(g_fw[id]));
	g_fw[id].alive  = 1;
	g_fw[id].parent = -1;
	g_fw[id].rot[3] = 1.0f;
	g_fw[id].scl[0] = g_fw[id].scl[1] = g_fw[id].scl[2] = 1.0f;
	rec("create|%d", id);
	return s7_make_integer(sc, id);
}

static s7_pointer st_entity_destroy(s7_scheme *sc, s7_pointer a)
{
	int id = (int)s7_integer(s7_car(a));

	rec("destroy|%d", id);
	if (id >= 0 && id < g_fw_count)
		g_fw[id].alive = 0;
	if (g_fw_sel == id)
		g_fw_sel = -1;
	return s7_unspecified(sc);
}

static s7_pointer st_entity_select(s7_scheme *sc, s7_pointer a)
{
	int id = (int)s7_integer(s7_car(a));

	rec("select|%d", id);
	g_fw_sel = id;
	return s7_unspecified(sc);
}

static s7_pointer st_entity_set_name(s7_scheme *sc, s7_pointer a)
{
	int         id = (int)s7_integer(s7_car(a));
	const char *nm = s7_is_string(s7_cadr(a)) ? s7_string(s7_cadr(a)) : "";

	rec("set-name|%d|%s", id, nm);
	if (id >= 0 && id < g_fw_count) {
		strncpy(g_fw[id].name, nm, sizeof(g_fw[id].name) - 1);
		g_fw[id].name[sizeof(g_fw[id].name) - 1] = '\0';
		g_fw[id].has_name = nm[0] != '\0';
	}
	return s7_unspecified(sc);
}

static s7_pointer st_entity_set_transform(s7_scheme *sc, s7_pointer a)
{
	(void)sc;
	rec("set-transform|%d", (int)s7_integer(s7_car(a)));
	return s7_unspecified(sc);
}

static s7_pointer st_entity_set_render_ref(s7_scheme *sc, s7_pointer a)
{
	rec("set-render-ref|%d|%u", (int)s7_integer(s7_car(a)),
	    (unsigned)s7_integer(s7_cadr(a)));
	return s7_unspecified(sc);
}

static s7_pointer st_entity_set_material_ref(s7_scheme *sc, s7_pointer a)
{
	rec("set-material-ref|%d|%u", (int)s7_integer(s7_car(a)),
	    (unsigned)s7_integer(s7_cadr(a)));
	return s7_unspecified(sc);
}

static s7_pointer st_entity_set_script_ref(s7_scheme *sc, s7_pointer a)
{
	rec("set-script-ref|%d|%u", (int)s7_integer(s7_car(a)),
	    (unsigned)s7_integer(s7_cadr(a)));
	return s7_unspecified(sc);
}

static s7_pointer st_gizmo_mode(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	return s7_make_integer(sc, g_gizmo);
}

static s7_pointer st_set_gizmo_mode(s7_scheme *sc, s7_pointer a)
{
	int m = (int)s7_integer(s7_car(a));

	rec("gizmo-mode|%d", m);
	g_gizmo = m;
	return s7_unspecified(sc);
}

/* ------------------------------------------------------------------ */
/* Assets-tab stub primitives + fixture (#402)                         */
/* ------------------------------------------------------------------ */

/* Fake authored asset catalog, driven by the mutating primitives. Mirrors
 * struct asset_info's shape closely enough for the Scheme logic under test:
 * type/kind/state/read_only/origin codes match asset_api.h. */
#define FA_MAX 10

struct fa_asset {
	int         alive;
	unsigned    id;
	char        path[64];
	int         type;
	int         kind;
	int         state;
	int         read_only;
	int         origin; /* 0 = FETCHED, 1 = AUTHORED */
	char        data[512];
	int         data_len;
};

static struct fa_asset g_fa[FA_MAX];
static int              g_fa_n;
static int              g_have_asset_api;
static int              g_have_asset_mut;
static int              g_create_fail;      /* next create()/clone() -> 0 */
static int              g_shader_save_ok = 1; /* krudd-asset-save-shader ret */
static int              g_script_save_ok = 1; /* krudd-asset-save-script ret */
static int              g_combo_pick = -1;  /* next imgui-combo result, or -1 */

/* Same id-keyed "what's being typed / just changed" simulation the World-tab
 * text/float stubs use, shared across every Assets text-like primitive
 * (single-line, enter-to-commit, and multiline all reduce to the same shape:
 * an id, an override string, and a boolean flag). */
static void asset_reset(void)
{
	memset(g_fa, 0, sizeof(g_fa));
	g_fa_n = 0;

	/* id 501: built-in mesh — read-only, a drag source in the browser. */
	g_fa[0].alive = 1; g_fa[0].id = 501;
	strcpy(g_fa[0].path, "builtin://mesh/cube");
	g_fa[0].type = 1; g_fa[0].kind = 1; g_fa[0].state = 1;
	g_fa[0].read_only = 1; g_fa[0].origin = 0;

	/* id 601: built-in shader — read-only, gets the Clone flow. */
	g_fa[1].alive = 1; g_fa[1].id = 601;
	strcpy(g_fa[1].path, "builtin://shader/scene");
	g_fa[1].type = 4; g_fa[1].kind = 1; g_fa[1].state = 1;
	g_fa[1].read_only = 1; g_fa[1].origin = 0;
	strcpy(g_fa[1].data, "(vertex ...) (fragment ...)");
	g_fa[1].data_len = (int)strlen(g_fa[1].data);

	/* id 701: authored text (markdown) — mutable, gets the editor. */
	g_fa[2].alive = 1; g_fa[2].id = 701;
	strcpy(g_fa[2].path, "notes.md");
	g_fa[2].type = 7; g_fa[2].kind = 0; g_fa[2].state = 1;
	g_fa[2].read_only = 0; g_fa[2].origin = 1;
	strcpy(g_fa[2].data, "# Hello");
	g_fa[2].data_len = (int)strlen(g_fa[2].data);

	/* id 801: authored shader — mutable, gets Save/Delete. */
	g_fa[3].alive = 1; g_fa[3].id = 801;
	strcpy(g_fa[3].path, "my.shader");
	g_fa[3].type = 4; g_fa[3].kind = 0; g_fa[3].state = 1;
	g_fa[3].read_only = 0; g_fa[3].origin = 1;
	strcpy(g_fa[3].data, "(vertex ...)");
	g_fa[3].data_len = (int)strlen(g_fa[3].data);

	/* id 901: authored material — mutable, gets the color picker. */
	g_fa[4].alive = 1; g_fa[4].id = 901;
	strcpy(g_fa[4].path, "red.mat");
	g_fa[4].type = 3; g_fa[4].kind = 0; g_fa[4].state = 1;
	g_fa[4].read_only = 0; g_fa[4].origin = 1;

	/* id 651: built-in script — read-only, gets the Clone flow. */
	g_fa[5].alive = 1; g_fa[5].id = 651;
	strcpy(g_fa[5].path, "builtin://script/spinner");
	g_fa[5].type = 8; g_fa[5].kind = 1; g_fa[5].state = 1;
	g_fa[5].read_only = 1; g_fa[5].origin = 0;
	strcpy(g_fa[5].data, "(script spinner (on-tick (self t) 0))");
	g_fa[5].data_len = (int)strlen(g_fa[5].data);

	/* id 851: authored script — mutable, gets Save/Delete. */
	g_fa[6].alive = 1; g_fa[6].id = 851;
	strcpy(g_fa[6].path, "my.script");
	g_fa[6].type = 8; g_fa[6].kind = 0; g_fa[6].state = 1;
	g_fa[6].read_only = 0; g_fa[6].origin = 1;
	strcpy(g_fa[6].data, "(script mine (on-tick (self t) 0))");
	g_fa[6].data_len = (int)strlen(g_fa[6].data);

	g_fa_n = 7;
	g_have_asset_api = 1;
	g_have_asset_mut = 1;
	g_create_fail    = 0;
	g_shader_save_ok = 1;
	g_script_save_ok = 1;

	g_click        = NULL;
	g_dis_top      = 0;
	g_input_id     = NULL;
	g_input_text   = NULL;
	g_input_commit = 0;
	g_combo_open   = NULL;
}

/* Reset the persistent Scheme-side Assets view state (kruddboard-assets-*)
 * between tests, the same job reset_state() does for the KRUDD tab's Scheme
 * state below. */
static void assets_scheme_reset(void)
{
	script_eval("(set! kruddboard-assets-sel 0)");
	script_eval("(set! kruddboard-assets-edit-id 0)");
	script_eval("(set! kruddboard-assets-edit-text \"\")");
	script_eval("(set! kruddboard-assets-shader-ok 'untried)");
	script_eval("(set! kruddboard-assets-script-ok 'untried)");
	script_eval("(set! kruddboard-assets-color-id 0)");
	script_eval("(set! kruddboard-assets-color (list 1.0 1.0 1.0 1.0))");
	script_eval("(set! kruddboard-assets-naming #f)");
	script_eval("(set! kruddboard-assets-new-name \"\")");
	script_eval("(set! kruddboard-assets-new-type 0)");
	script_eval("(set! kruddboard-assets-clone-src 0)");
	script_eval("(set! kruddboard-assets-clone-name \"\")");
	script_eval("(set! kruddboard-assets-clone-conflict #f)");
	script_eval("(set! kruddboard-assets-show-builtin #f)");
	g_combo_pick    = -1;
	g_float_id      = NULL;
	g_float_changed = 0;
}

static struct fa_asset *fa_find(unsigned id)
{
	int i;

	for (i = 0; i < g_fa_n; i++)
		if (g_fa[i].alive && g_fa[i].id == id)
			return &g_fa[i];
	return NULL;
}

static unsigned fa_next_id = 1001;

static s7_pointer st_assets(s7_scheme *sc, s7_pointer a)
{
	s7_pointer builtin = s7_nil(sc);
	s7_pointer project = s7_nil(sc);
	int        i;

	(void)a;
	if (!g_have_asset_api)
		return s7_f(sc);
	for (i = 0; i < g_fa_n; i++) {
		struct fa_asset *f = &g_fa[i];
		s7_pointer       row;

		if (!f->alive)
			continue;
		row = s7_list(sc, 7, s7_make_integer(sc, (s7_int)f->id),
			      s7_make_string(sc, f->path),
			      s7_make_integer(sc, f->type),
			      s7_make_integer(sc, f->kind),
			      s7_make_integer(sc, f->state),
			      s7_make_integer(sc, f->data_len),
			      s7_make_integer(sc, 0));
		if (f->read_only)
			builtin = s7_cons(sc, row, builtin);
		else
			project = s7_cons(sc, row, project);
	}
	return s7_list(sc, 2, s7_reverse(sc, builtin), s7_reverse(sc, project));
}

static s7_pointer st_asset_mut(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	return s7_make_boolean(sc, g_have_asset_mut);
}

static s7_pointer st_asset_info(s7_scheme *sc, s7_pointer a)
{
	unsigned          id = (unsigned)s7_integer(s7_car(a));
	struct fa_asset  *f  = fa_find(id);

	if (!f)
		return s7_f(sc);
	return s7_list(sc, 8, s7_make_string(sc, f->path),
		       s7_make_integer(sc, f->type),
		       s7_make_integer(sc, f->kind),
		       s7_make_integer(sc, f->state),
		       s7_make_integer(sc, f->data_len),
		       s7_make_integer(sc, 0),
		       s7_make_boolean(sc, f->read_only),
		       s7_make_integer(sc, f->origin));
}

static s7_pointer st_asset_describe(s7_scheme *sc, s7_pointer a)
{
	unsigned         id = (unsigned)s7_integer(s7_car(a));
	struct fa_asset *f  = fa_find(id);

	if (!f)
		return s7_nil(sc);
	return s7_list(sc, 1,
		s7_cons(sc, s7_make_string(sc, "path"),
			s7_make_string(sc, f->path)));
}

static s7_pointer st_asset_data(s7_scheme *sc, s7_pointer a)
{
	unsigned         id = (unsigned)s7_integer(s7_car(a));
	struct fa_asset *f  = fa_find(id);

	return s7_make_string(sc, f ? f->data : "");
}

static s7_pointer st_asset_color(s7_scheme *sc, s7_pointer a)
{
	unsigned         id = (unsigned)s7_integer(s7_car(a));
	struct fa_asset *f  = fa_find(id);
	float            v[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

	if (f && f->data_len >= (int)sizeof(v))
		memcpy(v, f->data, sizeof(v));
	return real_vec(sc, v, 4);
}

/* Mirrors the real shader_stages_from_source substring scan exactly, so the
 * Declaration display gets a meaningful test. */
static s7_pointer st_shader_stages(s7_scheme *sc, s7_pointer a)
{
	const char *src = s7_is_string(s7_car(a)) ? s7_string(s7_car(a)) : "";
	int         hv  = strstr(src, "(vertex")   != NULL;
	int         hf  = strstr(src, "(fragment") != NULL;

	if (hv && hf) return s7_make_string(sc, "vertex, fragment");
	if (hv)       return s7_make_string(sc, "vertex");
	if (hf)       return s7_make_string(sc, "fragment");
	return s7_make_string(sc, "");
}

static s7_pointer st_asset_save_text(s7_scheme *sc, s7_pointer a)
{
	unsigned         id  = (unsigned)s7_integer(s7_car(a));
	const char      *txt = s7_is_string(s7_cadr(a)) ? s7_string(s7_cadr(a)) : "";
	struct fa_asset *f   = fa_find(id);

	rec("save-text|%u|%s", id, txt);
	if (f) {
		strncpy(f->data, txt, sizeof(f->data) - 1);
		f->data_len = (int)strlen(f->data);
	}
	return s7_unspecified(sc);
}

static s7_pointer st_asset_save_shader(s7_scheme *sc, s7_pointer a)
{
	unsigned         id  = (unsigned)s7_integer(s7_car(a));
	const char      *txt = s7_is_string(s7_cadr(a)) ? s7_string(s7_cadr(a)) : "";
	struct fa_asset *f   = fa_find(id);

	rec("save-shader|%u|%s", id, txt);
	if (!g_shader_save_ok)
		return s7_f(sc);
	if (f) {
		strncpy(f->data, txt, sizeof(f->data) - 1);
		f->data_len = (int)strlen(f->data);
	}
	return s7_t(sc);
}

static s7_pointer st_asset_save_material(s7_scheme *sc, s7_pointer a)
{
	s7_pointer       p  = a;
	unsigned         id = (unsigned)s7_integer(s7_car(p)); p = s7_cdr(p);
	float            v[4];
	struct fa_asset *f;
	int              i;

	for (i = 0; i < 4 && s7_is_pair(p); i++) {
		v[i] = (float)s7_number_to_real(sc, s7_car(p));
		p    = s7_cdr(p);
	}
	rec("save-material|%u|%.2f,%.2f,%.2f,%.2f", id, v[0], v[1], v[2], v[3]);
	f = fa_find(id);
	if (f) {
		memcpy(f->data, v, sizeof(v));
		f->data_len = (int)sizeof(v);
	}
	return s7_unspecified(sc);
}

static s7_pointer st_asset_delete(s7_scheme *sc, s7_pointer a)
{
	unsigned         id = (unsigned)s7_integer(s7_car(a));
	struct fa_asset *f  = fa_find(id);

	rec("delete|%u", id);
	if (f)
		f->alive = 0;
	return s7_unspecified(sc);
}

static unsigned fa_create(const char *path, int type, const char *data,
			  int data_len, int read_only, int origin)
{
	struct fa_asset *f;

	if (g_create_fail || g_fa_n >= FA_MAX)
		return 0;
	f = &g_fa[g_fa_n++];
	memset(f, 0, sizeof(*f));
	f->alive     = 1;
	f->id        = fa_next_id++;
	f->type      = type;
	f->read_only = read_only;
	f->origin    = origin;
	f->state     = 1;
	strncpy(f->path, path, sizeof(f->path) - 1);
	if (data && data_len > 0) {
		memcpy(f->data, data, (size_t)data_len);
		f->data_len = data_len;
	}
	return f->id;
}

static s7_pointer st_asset_create_text(s7_scheme *sc, s7_pointer a)
{
	const char *path = s7_is_string(s7_car(a)) ? s7_string(s7_car(a)) : "";
	unsigned    id;

	rec("create-text|%s", path);
	id = fa_create(path, 7, "", 0, 0, 1);
	return s7_make_integer(sc, (s7_int)id);
}

static s7_pointer st_asset_create_shader(s7_scheme *sc, s7_pointer a)
{
	const char *path = s7_is_string(s7_car(a)) ? s7_string(s7_car(a)) : "";
	unsigned    id;

	rec("create-shader|%s", path);
	id = fa_create(path, 4, "(vertex seed)", 13, 0, 1);
	return s7_make_integer(sc, (s7_int)id);
}

static s7_pointer st_asset_create_material(s7_scheme *sc, s7_pointer a)
{
	const char *path = s7_is_string(s7_car(a)) ? s7_string(s7_car(a)) : "";
	static const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	unsigned    id;

	rec("create-material|%s", path);
	id = fa_create(path, 3, (const char *)white, (int)sizeof(white), 0, 1);
	return s7_make_integer(sc, (s7_int)id);
}

static s7_pointer st_asset_clone_shader(s7_scheme *sc, s7_pointer a)
{
	const char *name = s7_is_string(s7_car(a)) ? s7_string(s7_car(a)) : "";
	const char *txt  = s7_is_string(s7_cadr(a)) ? s7_string(s7_cadr(a)) : "";
	unsigned    id;

	rec("clone-shader|%s|%s", name, txt);
	id = fa_create(name, 4, txt, (int)strlen(txt), 0, 1);
	return s7_make_integer(sc, (s7_int)id);
}

/* Mirrors the real script_hooks_from_source substring scan, so the script
 * Declaration display gets a meaningful test. */
static s7_pointer st_script_hooks(s7_scheme *sc, s7_pointer a)
{
	const char *src = s7_is_string(s7_car(a)) ? s7_string(s7_car(a)) : "";
	int         hb  = strstr(src, "(on-begin")   != NULL;
	int         ht  = strstr(src, "(on-tick")    != NULL;
	int         hd  = strstr(src, "(on-destroy") != NULL;
	char        buf[64];
	int         n   = 0;

	buf[0] = '\0';
	if (hb) { strcpy(buf, "on-begin"); n++; }
	if (ht) { if (n) strcat(buf, ", "); strcat(buf, "on-tick"); n++; }
	if (hd) { if (n) strcat(buf, ", "); strcat(buf, "on-destroy"); }
	return s7_make_string(sc, buf);
}

static s7_pointer st_asset_save_script(s7_scheme *sc, s7_pointer a)
{
	unsigned         id  = (unsigned)s7_integer(s7_car(a));
	const char      *txt = s7_is_string(s7_cadr(a)) ? s7_string(s7_cadr(a)) : "";
	struct fa_asset *f   = fa_find(id);

	rec("save-script|%u|%s", id, txt);
	if (!g_script_save_ok)
		return s7_f(sc);
	if (f) {
		strncpy(f->data, txt, sizeof(f->data) - 1);
		f->data_len = (int)strlen(f->data);
	}
	return s7_t(sc);
}

#define SCRIPT_SEED "(script seed (on-tick (self t) 0))"

static s7_pointer st_asset_create_script(s7_scheme *sc, s7_pointer a)
{
	const char *path = s7_is_string(s7_car(a)) ? s7_string(s7_car(a)) : "";
	unsigned    id;

	rec("create-script|%s", path);
	id = fa_create(path, 8, SCRIPT_SEED, (int)strlen(SCRIPT_SEED), 0, 1);
	return s7_make_integer(sc, (s7_int)id);
}

static s7_pointer st_asset_clone_script(s7_scheme *sc, s7_pointer a)
{
	const char *name = s7_is_string(s7_car(a)) ? s7_string(s7_car(a)) : "";
	const char *txt  = s7_is_string(s7_cadr(a)) ? s7_string(s7_cadr(a)) : "";
	unsigned    id;

	rec("clone-script|%s|%s", name, txt);
	id = fa_create(name, 8, txt, (int)strlen(txt), 0, 1);
	return s7_make_integer(sc, (s7_int)id);
}

static s7_pointer st_asset_clone_material(s7_scheme *sc, s7_pointer a)
{
	s7_pointer  p    = a;
	const char *name = s7_is_string(s7_car(p)) ? s7_string(s7_car(p)) : "";
	float       v[4];
	unsigned    id;
	int         i;

	p = s7_cdr(p);
	for (i = 0; i < 4 && s7_is_pair(p); i++) {
		v[i] = (float)s7_number_to_real(sc, s7_car(p));
		p    = s7_cdr(p);
	}
	rec("clone-material|%s|%.2f,%.2f,%.2f,%.2f", name, v[0], v[1], v[2], v[3]);
	id = fa_create(name, 3, (const char *)v, (int)sizeof(v), 0, 1);
	return s7_make_integer(sc, (s7_int)id);
}

static s7_pointer st_md_preview(s7_scheme *sc, s7_pointer a)
{
	const char *txt = s7_is_string(s7_car(a)) ? s7_string(s7_car(a)) : "";

	rec("md-preview|%s", txt);
	return s7_unspecified(sc);
}

static s7_pointer st_button(s7_scheme *sc, s7_pointer a)
{
	const char *l = s7_string(s7_car(a));

	rec("btn|%s", l);
	return s7_make_boolean(sc, clicked(l) && !disabled_now());
}

static s7_pointer st_input_text_enter(s7_scheme *sc, s7_pointer a)
{
	const char *id  = s7_string(s7_car(a));
	const char *cur = s7_is_string(s7_cadr(a)) ? s7_string(s7_cadr(a)) : "";
	int         entered = 0;

	if (g_input_id && strcmp(g_input_id, id) == 0) {
		if (g_input_text)
			cur = g_input_text;
		entered = g_input_commit;
	}
	rec("input-enter|%s|%s", id, cur);
	return s7_cons(sc, s7_make_string(sc, cur), s7_make_boolean(sc, entered));
}

static s7_pointer st_input_text_multiline(s7_scheme *sc, s7_pointer a)
{
	s7_pointer  p    = a;
	const char *id   = s7_string(s7_car(p)); p = s7_cdr(p);
	const char *cur  = s7_is_string(s7_car(p)) ? s7_string(s7_car(p)) : "";
	int         changed = 0;

	if (g_input_id && strcmp(g_input_id, id) == 0) {
		if (g_input_text)
			cur = g_input_text;
		changed = g_input_commit;
	}
	rec("input-ml|%s|%s", id, cur);
	return s7_cons(sc, s7_make_string(sc, cur), s7_make_boolean(sc, changed));
}

static s7_pointer st_combo(s7_scheme *sc, s7_pointer a)
{
	const char *id   = s7_string(s7_car(a));
	int         cur  = (int)s7_integer(s7_caddr(a));

	rec("combo|%s|%d", id, cur);
	return s7_make_integer(sc, g_combo_pick >= 0 ? g_combo_pick : cur);
}

static s7_pointer st_color_edit4(s7_scheme *sc, s7_pointer a)
{
	const char *id = s7_string(s7_car(a));
	int         changed = g_float_id && strcmp(g_float_id, id) == 0
		&& g_float_changed;

	rec("coloredit|%s", id);
	return s7_cons(sc, s7_cadr(a), s7_make_boolean(sc, changed));
}

static s7_pointer st_mesh_drag_source(s7_scheme *sc, s7_pointer a)
{
	rec("drag-source|%lld|%s", (long long)s7_integer(s7_car(a)),
	    s7_string(s7_cadr(a)));
	return s7_unspecified(sc);
}

/* ------------------------------------------------------------------ */
/* Harness setup                                                       */
/* ------------------------------------------------------------------ */

static void def(s7_scheme *sc, const char *name, s7_function fn, int nargs)
{
	s7_define_function(sc, name, fn, nargs, 0, false, "stub");
}

static s7_scheme *setup(void)
{
	s7_scheme *sc = script_s7();

	assert(sc);
	def(sc, "imgui-text", st_text, 1);
	def(sc, "imgui-text-disabled", st_text_disabled, 1);
	def(sc, "imgui-text-colored", st_text_colored, 5);
	def(sc, "imgui-small-button", st_small_button, 1);
	def(sc, "imgui-same-line", st_same_line, 0);
	def(sc, "imgui-checkbox", st_checkbox, 2);
	def(sc, "imgui-separator", st_separator, 0);
	s7_define_function(sc, "imgui-collapsing-header",
			   st_collapsing_header, 1, 1, false, "stub");
	def(sc, "imgui-begin-table", st_begin_table, 2);
	def(sc, "imgui-table-setup-column", st_setup_column, 1);
	def(sc, "imgui-table-headers-row", st_headers_row, 0);
	def(sc, "imgui-table-next-row", st_next_row, 0);
	def(sc, "imgui-table-next-column", st_next_column, 0);
	def(sc, "imgui-end-table", st_end_table, 0);
	def(sc, "imgui-begin-child", st_begin_child, 3);
	def(sc, "imgui-end-child", st_end_child, 0);
	def(sc, "imgui-set-scroll-here-y", st_scroll_here, 1);
	def(sc, "imgui-viewport-work-height", st_work_height, 0);
	def(sc, "krudd-stats", st_stats, 0);
	def(sc, "krudd-subsystems", st_subsystems, 0);
	def(sc, "krudd-log-history", st_log_history, 0);

	def(sc, "imgui-begin-disabled", st_begin_disabled, 1);
	def(sc, "imgui-end-disabled", st_end_disabled, 0);
	def(sc, "imgui-set-next-item-width", st_set_next_item_width, 1);
	def(sc, "imgui-input-text", st_input_text, 2);
	def(sc, "imgui-input-float3", st_input_float3, 2);
	def(sc, "imgui-input-float4", st_input_float4, 2);
	def(sc, "imgui-begin-combo", st_begin_combo, 2);
	def(sc, "imgui-end-combo", st_end_combo, 0);
	def(sc, "imgui-selectable", st_selectable, 3);
	def(sc, "imgui-set-item-default-focus", st_default_focus, 0);
	def(sc, "imgui-calc-text-width", st_calc_text_width, 1);
	def(sc, "imgui-same-line-right", st_same_line_right, 1);
	def(sc, "imgui-push-style-color-button", st_push_btn_color, 4);
	def(sc, "imgui-pop-style-color", st_pop_color, 0);
	def(sc, "imgui-begin-table-plain", st_begin_table_plain, 2);
	def(sc, "imgui-table-setup-column-fixed", st_setup_column_fixed, 2);
	def(sc, "krudd-world-caps", st_world_caps, 0);
	def(sc, "krudd-selected", st_selected, 0);
	def(sc, "krudd-world-entities", st_world_entities, 0);
	def(sc, "krudd-entity-inspect", st_entity_inspect, 1);
	def(sc, "krudd-mesh-assets", st_mesh_assets, 0);
	def(sc, "krudd-material-assets", st_material_assets, 0);
	def(sc, "krudd-script-assets", st_script_assets, 0);
	def(sc, "krudd-asset-find", st_asset_find, 1);
	def(sc, "krudd-entity-create", st_entity_create, 0);
	def(sc, "krudd-entity-destroy", st_entity_destroy, 1);
	def(sc, "krudd-entity-select", st_entity_select, 1);
	def(sc, "krudd-entity-set-name", st_entity_set_name, 2);
	def(sc, "krudd-entity-set-transform", st_entity_set_transform, 4);
	def(sc, "krudd-entity-set-render-ref", st_entity_set_render_ref, 2);
	def(sc, "krudd-entity-set-material-ref", st_entity_set_material_ref, 2);
	def(sc, "krudd-entity-set-script-ref", st_entity_set_script_ref, 2);
	def(sc, "krudd-gizmo-mode", st_gizmo_mode, 0);
	def(sc, "krudd-set-gizmo-mode", st_set_gizmo_mode, 1);

	def(sc, "imgui-button", st_button, 1);
	def(sc, "imgui-input-text-enter", st_input_text_enter, 2);
	def(sc, "imgui-input-text-multiline", st_input_text_multiline, 4);
	def(sc, "imgui-combo", st_combo, 3);
	def(sc, "imgui-color-edit4", st_color_edit4, 2);
	def(sc, "imgui-mesh-drag-source", st_mesh_drag_source, 2);
	def(sc, "krudd-assets", st_assets, 0);
	def(sc, "krudd-asset-mut?", st_asset_mut, 0);
	def(sc, "krudd-asset-info", st_asset_info, 1);
	def(sc, "krudd-asset-describe", st_asset_describe, 1);
	def(sc, "krudd-asset-data", st_asset_data, 1);
	def(sc, "krudd-asset-color", st_asset_color, 1);
	def(sc, "krudd-shader-stages", st_shader_stages, 1);
	def(sc, "krudd-asset-save-text", st_asset_save_text, 2);
	def(sc, "krudd-asset-save-shader", st_asset_save_shader, 2);
	def(sc, "krudd-asset-save-material", st_asset_save_material, 5);
	def(sc, "krudd-asset-delete", st_asset_delete, 1);
	def(sc, "krudd-asset-create-text", st_asset_create_text, 1);
	def(sc, "krudd-asset-create-shader", st_asset_create_shader, 1);
	def(sc, "krudd-asset-create-material", st_asset_create_material, 1);
	def(sc, "krudd-asset-clone-shader", st_asset_clone_shader, 2);
	def(sc, "krudd-script-hooks", st_script_hooks, 1);
	def(sc, "krudd-asset-save-script", st_asset_save_script, 2);
	def(sc, "krudd-asset-create-script", st_asset_create_script, 1);
	def(sc, "krudd-asset-clone-script", st_asset_clone_script, 2);
	def(sc, "krudd-asset-clone-material", st_asset_clone_material, 5);
	def(sc, "krudd-md-preview", st_md_preview, 2);

	assert(script_eval(KRUDDBOARD_SCM) == 0);
	assert(script_eval(ASSETS_SCM) == 0);
	return sc;
}

/* Call a nullary panel procedure the image defines. */
static void draw(const char *proc)
{
	s7_scheme *sc = script_s7();
	s7_pointer fn = s7_name_to_value(sc, proc);

	assert(s7_is_procedure(fn));
	s7_call(sc, fn, s7_nil(sc));
}

/* Reset the persistent Scheme view state between frames. */
static void reset_state(void)
{
	script_eval("(set! kruddboard-log-filter 0)");
	script_eval("(set! kruddboard-log-autoscroll #t)");
	g_click         = NULL;
	g_subsys_absent = 0;
	g_log_absent    = 0;
}

static int log_filter(void)
{
	return (int)s7_integer(
		s7_name_to_value(script_s7(), "kruddboard-log-filter"));
}

static int assets_sel(void)
{
	return (int)s7_integer(
		s7_name_to_value(script_s7(), "kruddboard-assets-sel"));
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

/* The subsystems table lays out header + one row per subsystem, in order. */
static void test_subsystems_table(void)
{
	reset_state();
	rec_reset();
	draw("kruddboard-draw-subsystems");

	assert(rec_has("table-begin|##subsys|3"));
	assert(rec_has("col|Name") && !rec_has("col|WASM Size"));
	assert(rec_has("headers"));
	assert(rec_count("row") == 3);
	assert(rec_has("text|log"));
	assert(rec_has("text|memory"));
	assert(rec_has("text|kruddboard"));
	assert(rec_has("table-end"));
}

/* yes/- for API and Tick; no WASM size column is drawn. */
static void test_subsystems_cells(void)
{
	reset_state();
	rec_reset();
	draw("kruddboard-draw-subsystems");

	assert(rec_has("text|yes"));      /* an API/Tick that is present */
	assert(rec_has("text|-"));        /* an API/Tick that is absent   */
	assert(!rec_has("text|1024"));    /* WASM size no longer drawn    */
	assert(rec_count("disabled|-") == 0); /* no dimmed size cells     */
}

/* No manager -> the dimmed unavailable line, and no table. */
static void test_subsystems_absent(void)
{
	reset_state();
	g_subsys_absent = 1;
	rec_reset();
	draw("kruddboard-draw-subsystems");

	assert(rec_has("disabled|(subsystem manager unavailable)"));
	assert(!rec_has("table-begin"));
}

/* Default filter (DEBUG) shows every line, each in its level colour. */
static void test_log_all_levels_colored(void)
{
	reset_state();
	rec_reset();
	draw("kruddboard-draw-log");

	assert(rec_count("colored|") == 4);
	assert(rec_has("colored|0.60,0.60,0.60,1.00|dbg line"));  /* grey  */
	assert(rec_has("colored|1.00,1.00,1.00,1.00|info line")); /* white */
	assert(rec_has("colored|1.00,0.80,0.20,1.00|warn line")); /* yellow*/
	assert(rec_has("colored|1.00,0.30,0.30,1.00|err line"));  /* red   */
	assert(rec_has("child-begin|##logscroll"));
	assert(rec_has("scrollhere|1.00"));   /* autoscroll on by default */
	assert(rec_has("child-end"));
}

/* Clicking ERROR raises the filter, and only error+ lines draw that frame. */
static void test_log_filter_click(void)
{
	reset_state();
	g_click = "ERROR";
	rec_reset();
	draw("kruddboard-draw-log");
	g_click = NULL;

	assert(log_filter() == 3);
	assert(rec_count("colored|") == 1);
	assert(rec_has("colored|1.00,0.30,0.30,1.00|err line"));
}

/* Toggling Auto-scroll off suppresses the scroll-to-bottom that frame. */
static void test_log_autoscroll_toggle(void)
{
	reset_state();
	g_click = "Auto-scroll";
	rec_reset();
	draw("kruddboard-draw-log");
	g_click = NULL;

	assert(!rec_has("scrollhere|"));
}

/* No log subsystem -> the dimmed unavailable line, no controls or child. */
static void test_log_absent(void)
{
	reset_state();
	g_log_absent = 1;
	rec_reset();
	draw("kruddboard-draw-log");

	assert(rec_has("disabled|(log unavailable)"));
	assert(!rec_has("child-begin"));
	assert(!rec_has("button|DEBUG"));
}

/* The KRUDD tab composes the three sections under headers, in order. */
static void test_krudd_composition(void)
{
	reset_state();
	rec_reset();
	draw("kruddboard-draw-krudd");

	assert(rec_index("header|Frame Stats") >= 0);
	assert(rec_index("header|Frame Stats") < rec_index("header|Subsystems"));
	assert(rec_index("header|Subsystems") < rec_index("header|Log"));
	assert(rec_has("header|Frame Stats|1")); /* defaults open   */
	assert(rec_has("header|Subsystems|0"));  /* starts rolled up */
	assert(rec_has("header|Log|1"));         /* defaults open   */
	assert(rec_has("table-begin|##subsys|3")); /* subsystems drawn */
	assert(rec_has("child-begin|##logscroll")); /* log drawn */
}

/* The scene header draws the title and a right-aligned, disabled "Save As...". */
static void test_world_header(void)
{
	fw_reset();
	rec_reset();
	script_eval("(kruddboard-draw-world-header)");

	assert(rec_has("text|Untitled Scene"));
	assert(rec_has("sameline-right|"));
	assert(rec_index("dis-begin|1") < rec_index("button|Save As..."));
	assert(rec_index("button|Save As...") < rec_index("dis-end"));
}

/* The entity list: a selectable per live entity (selected one flagged) + delete. */
static void test_world_entity_list(void)
{
	fw_reset();
	rec_reset();
	script_eval("(kruddboard-draw-world-entities #t)");

	assert(rec_has("button|+ Entity"));
	assert(rec_has("table-begin|##entlist|2"));
	assert(rec_has("selectable|Cube##e0|1"));       /* selected        */
	assert(rec_has("selectable|entity 1##e1|0"));   /* fallback name   */
	assert(rec_has("button|x##d0"));
	assert(rec_has("button|x##d1"));
	assert(rec_has("table-end"));
}

/* An empty world shows the dimmed placeholder and draws no table. */
static void test_world_no_entities(void)
{
	fw_reset();
	g_fw_count = 0;
	rec_reset();
	script_eval("(kruddboard-draw-world-entities #t)");

	assert(rec_has("disabled|(no entities)"));
	assert(!rec_has("table-begin|##entlist"));
}

/* "+ Entity" appends, names the new entity, and selects it — in that order. */
static void test_world_create(void)
{
	fw_reset();
	g_click = "+ Entity";
	rec_reset();
	script_eval("(kruddboard-draw-world-entities #t)");
	g_click = NULL;

	assert(rec_has("create|2"));
	assert(rec_has("set-name|2|Entity"));
	assert(rec_has("select|2"));
	assert(rec_index("create|2") < rec_index("set-name|2|Entity"));
	assert(rec_index("set-name|2|Entity") < rec_index("select|2"));
}

/* With no scene api the create button is disabled, so a click does nothing. */
static void test_world_create_disabled(void)
{
	fw_reset();
	g_have_entity = 0;
	g_click = "+ Entity";
	rec_reset();
	script_eval("(kruddboard-draw-world-entities #f)");
	g_click = NULL;

	assert(!rec_has("create|"));
	assert(rec_has("disabled|(no entities)"));
}

/* The row delete button tombstones that entity. */
static void test_world_delete(void)
{
	fw_reset();
	g_click = "x##d1";
	rec_reset();
	script_eval("(kruddboard-draw-world-entities #t)");
	g_click = NULL;

	assert(rec_has("destroy|1"));
}

/* Clicking a row's selectable selects that entity. */
static void test_world_select(void)
{
	fw_reset();
	g_click = "entity 1##e1";
	rec_reset();
	script_eval("(kruddboard-draw-world-entities #t)");
	g_click = NULL;

	assert(rec_has("select|1"));
}

/* The inspector renders name, transform, details, and both binding combos. */
static void test_world_inspector(void)
{
	fw_reset();
	rec_reset();
	script_eval("(kruddboard-draw-world-inspector (list #t #t))");

	assert(rec_has("input-text|##ename|Cube"));
	assert(rec_has("table-plain|##xform"));
	assert(rec_has("input-f3|##pos"));
	assert(rec_has("input-f4|##rot"));
	assert(rec_has("input-f3|##scl"));
	assert(rec_has("text|Entity ID"));
	assert(rec_has("text|(root)"));
	assert(rec_has("text|Transform, Name, Render")); /* name + render, no mat */
	assert(rec_has("combo|##meshsel|cube.mesh"));    /* resolved binding      */
	assert(rec_has("combo|##materialsel|(none)"));   /* unbound material      */
	assert(rec_has("combo|##scriptsel|(none)"));     /* unbound script        */
}

/* Nothing selected -> the dimmed placeholder, and no editable widgets. */
static void test_world_inspector_empty(void)
{
	fw_reset();
	g_fw_sel = -1;
	rec_reset();
	script_eval("(kruddboard-draw-world-inspector (list #t #t))");

	assert(rec_has("disabled|(nothing selected)"));
	assert(!rec_has("input-text|##ename"));
}

/* Committing the name field pushes the new name through the mutator. */
static void test_world_name_commit(void)
{
	fw_reset();
	g_input_id     = "##ename";
	g_input_text   = "Renamed";
	g_input_commit = 1;
	rec_reset();
	script_eval("(kruddboard-draw-world-inspector (list #t #t))");
	g_input_id = NULL;
	g_input_commit = 0;

	assert(rec_has("set-name|0|Renamed"));
}

/* A name still being typed (not yet committed) writes nothing back. */
static void test_world_name_uncommitted(void)
{
	fw_reset();
	g_input_id     = "##ename";
	g_input_text   = "Typing";
	g_input_commit = 0;
	rec_reset();
	script_eval("(kruddboard-draw-world-inspector (list #t #t))");
	g_input_id = NULL;

	assert(!rec_has("set-name|"));
}

/* Editing a transform field writes the whole transform back. */
static void test_world_transform_edit(void)
{
	fw_reset();
	g_float_id      = "##pos";
	g_float_changed = 1;
	rec_reset();
	script_eval("(kruddboard-draw-world-inspector (list #t #t))");
	g_float_id = NULL;
	g_float_changed = 0;

	assert(rec_has("set-transform|0"));
}

/* Choosing a mesh from the open combo rebinds render_ref. */
static void test_world_mesh_bind(void)
{
	fw_reset();
	g_combo_open = "##meshsel";
	g_click = "sphere.mesh##m102";
	rec_reset();
	script_eval("(kruddboard-draw-world-inspector (list #t #t))");
	g_click = NULL;
	g_combo_open = NULL;

	assert(rec_has("set-render-ref|0|102"));
}

/* The combo's "(none)" entry unbinds the mesh (render_ref 0). */
static void test_world_mesh_unbind(void)
{
	fw_reset();
	g_combo_open = "##meshsel";
	g_click = "(none)";
	rec_reset();
	script_eval("(kruddboard-draw-world-inspector (list #t #t))");
	g_click = NULL;
	g_combo_open = NULL;

	assert(rec_has("set-render-ref|0|0"));
}

/* Binding combos are disabled without the asset api, so a click does nothing. */
static void test_world_bind_disabled(void)
{
	fw_reset();
	g_have_asset = 0;
	g_combo_open = "##meshsel";
	g_click = "sphere.mesh##m102";
	rec_reset();
	script_eval("(kruddboard-draw-world-inspector (list #t #f))");
	g_click = NULL;
	g_combo_open = NULL;

	assert(!rec_has("set-render-ref|"));
}

/* Choosing a script from the open combo rebinds script_ref. */
static void test_world_script_bind(void)
{
	fw_reset();
	g_combo_open = "##scriptsel";
	g_click = "orbit.kscm##m301";
	rec_reset();
	script_eval("(kruddboard-draw-world-inspector (list #t #t))");
	g_click = NULL;
	g_combo_open = NULL;

	assert(rec_has("set-script-ref|0|301"));
}

/* The combo's "(none)" entry unbinds the script (script_ref 0). */
static void test_world_script_unbind(void)
{
	fw_reset();
	g_fw[0].has_script = 1;
	g_fw[0].script_ref = 301;
	g_combo_open = "##scriptsel";
	g_click = "(none)";
	rec_reset();
	script_eval("(kruddboard-draw-world-inspector (list #t #t))");
	g_click = NULL;
	g_combo_open = NULL;

	assert(rec_has("set-script-ref|0|0"));
}

/* A bound script shows its resolved path and the "Script" component tag. */
static void test_world_script_resolved(void)
{
	fw_reset();
	g_fw[0].has_script = 1;
	g_fw[0].script_ref = 301;
	rec_reset();
	script_eval("(kruddboard-draw-world-inspector (list #t #t))");

	assert(rec_has("text|Transform, Name, Render, Script"));
	assert(rec_has("combo|##scriptsel|orbit.kscm"));
}

/* The tool chips highlight the active tool and switch it on a click. */
static void test_world_gizmo_chips(void)
{
	fw_reset();
	g_click = "Rotate";
	rec_reset();
	script_eval("(kruddboard-draw-gizmo-chips)");
	g_click = NULL;

	assert(rec_has("text|Tool"));
	assert(rec_has("push-btn-color|0.27,0.43,0.67")); /* Move active */
	assert(rec_has("gizmo-mode|1"));
}

/* The whole tab composes header, entities, tool chips, then inspector, in order. */
static void test_world_composition(void)
{
	fw_reset();
	rec_reset();
	script_eval("(kruddboard-draw-world)");

	assert(rec_has("text|Untitled Scene"));
	assert(rec_index("header|Entities") >= 0);
	assert(rec_index("header|Entities") < rec_index("text|Tool"));
	assert(rec_index("text|Tool") < rec_index("header|Inspector"));
}

/* ------------------------------------------------------------------ */
/* Assets-tab tests (#402)                                             */
/* ------------------------------------------------------------------ */

/* No asset api: the whole tab shows the dimmed placeholder. */
static void test_assets_unavailable(void)
{
	asset_reset();
	assets_scheme_reset();
	g_have_asset_api = 0;
	rec_reset();
	script_eval("(kruddboard-draw-assets)");

	assert(rec_has("disabled|(assets unavailable)"));
}

/* An empty catalog (api present, no rows) shows the other placeholder. */
static void test_assets_no_assets(void)
{
	asset_reset();
	assets_scheme_reset();
	g_fa_n = 0;
	rec_reset();
	script_eval("(kruddboard-draw-assets)");

	assert(rec_has("header|Browser"));
	assert(rec_has("disabled|(no assets)"));
}

/* The browser table lists project assets by default; the BUILT-IN group is
 * hidden behind a checkbox that starts unchecked (#420 — builtin:// rows are
 * just noise in most sessions). */
static void test_assets_browser_table(void)
{
	asset_reset();
	assets_scheme_reset();
	rec_reset();
	script_eval("(kruddboard-draw-assets)");

	assert(rec_has("header|Browser"));
	assert(rec_has("checkbox|Show built-in assets|0"));
	assert(rec_has("table-begin|##assets|6"));
	assert(!rec_has("disabled|-- BUILT-IN (read-only) --"));
	assert(rec_has("disabled|-- PROJECT --"));
	assert(!rec_has("selectable|builtin://mesh/cube|0"));
	assert(rec_has("selectable|my.shader|0"));
	assert(!rec_has("drag-source|501|builtin://mesh/cube"));
	assert(!rec_has("colored|1.00,0.60,0.20,1.00|RO"));
}

/* Checking "Show built-in assets" reveals the BUILT-IN group and its rows,
 * including the mesh row's drag source. */
static void test_assets_browser_table_show_builtin(void)
{
	asset_reset();
	assets_scheme_reset();
	g_click = "Show built-in assets";
	rec_reset();
	script_eval("(kruddboard-draw-assets)");
	g_click = NULL;

	assert(rec_has("checkbox|Show built-in assets|0"));
	assert(rec_has("disabled|-- BUILT-IN (read-only) --"));
	assert(rec_has("disabled|-- PROJECT --"));
	assert(rec_has("selectable|builtin://mesh/cube|0"));
	assert(rec_has("selectable|my.shader|0"));
	assert(rec_has("drag-source|501|builtin://mesh/cube"));
	assert(!rec_has("drag-source|601|"));
	assert(rec_has("colored|1.00,0.60,0.20,1.00|RO"));
}

/* Clicking a row's selectable opens that asset in the inspector next draw. */
static void test_assets_row_select(void)
{
	asset_reset();
	assets_scheme_reset();
	g_click = "notes.md";
	rec_reset();
	script_eval("(kruddboard-draw-assets)");
	g_click = NULL;

	assert(assets_sel() == 701);
}

static void test_assets_new_asset_button(void)
{
	asset_reset();
	assets_scheme_reset();
	rec_reset();
	script_eval("(kruddboard-draw-assets)");

	assert(rec_has("btn|New Asset"));
}

/* With no mutation api, the New Asset button never draws. */
static void test_assets_new_asset_hidden_without_mut(void)
{
	asset_reset();
	assets_scheme_reset();
	g_have_asset_mut = 0;
	rec_reset();
	script_eval("(kruddboard-draw-assets)");

	assert(!rec_has("btn|New Asset"));
}

/* "New Asset" -> fill name, leave type at its Text default -> Create. */
static void test_assets_create_text(void)
{
	asset_reset();
	assets_scheme_reset();
	g_click = "New Asset";
	script_eval("(kruddboard-draw-assets)");
	g_click = "Create";
	g_input_id = "name"; g_input_text = "new.md"; g_input_commit = 0;
	rec_reset();
	script_eval("(kruddboard-draw-assets)");
	g_click = NULL; g_input_id = NULL;

	assert(rec_has("create-text|new.md"));
	assert(assets_sel() != 0);
}

/* Picking "Shader" from the type combo dispatches to the shader creator. */
static void test_assets_create_shader(void)
{
	asset_reset();
	assets_scheme_reset();
	g_click = "New Asset";
	script_eval("(kruddboard-draw-assets)");
	g_click = "Create";
	g_input_id = "name"; g_input_text = "new.shader"; g_input_commit = 0;
	g_combo_pick = 1; /* Shader */
	rec_reset();
	script_eval("(kruddboard-draw-assets)");
	g_click = NULL; g_input_id = NULL; g_combo_pick = -1;

	assert(rec_has("create-shader|new.shader"));
}

/* Cancel closes the form without creating anything; the button reappears. */
static void test_assets_new_asset_cancel(void)
{
	asset_reset();
	assets_scheme_reset();
	g_click = "New Asset";
	script_eval("(kruddboard-draw-assets)");
	g_click = "Cancel";
	rec_reset();
	script_eval("(kruddboard-draw-assets)");
	g_click = NULL;

	assert(rec_has("btn|Cancel"));
	assert(!rec_has("create-text|"));

	rec_reset();
	script_eval("(kruddboard-draw-assets)");
	assert(rec_has("btn|New Asset"));
}

/* Authored text: source box, live preview, Save/Delete. */
static void test_assets_text_editor(void)
{
	asset_reset();
	assets_scheme_reset();
	script_eval("(set! kruddboard-assets-sel 701)");
	rec_reset();
	script_eval("(kruddboard-draw-assets)");

	assert(rec_has("text|notes.md"));
	assert(rec_has("header|Source"));
	assert(rec_has("input-ml|##md|# Hello"));
	assert(rec_has("header|Preview"));
	assert(rec_has("md-preview|# Hello"));
	assert(rec_has("btn|Save"));
	assert(rec_has("btn|Delete"));
}

static void test_assets_text_save(void)
{
	asset_reset();
	assets_scheme_reset();
	script_eval("(set! kruddboard-assets-sel 701)");
	g_click = "Save";
	g_input_id = "##md"; g_input_text = "# Edited"; g_input_commit = 1;
	rec_reset();
	script_eval("(kruddboard-draw-assets)");
	g_click = NULL; g_input_id = NULL;

	assert(rec_has("save-text|701|# Edited"));
}

static void test_assets_text_delete(void)
{
	asset_reset();
	assets_scheme_reset();
	script_eval("(set! kruddboard-assets-sel 701)");
	g_click = "Delete";
	rec_reset();
	script_eval("(kruddboard-draw-assets)");
	g_click = NULL;

	assert(rec_has("delete|701"));
	assert(assets_sel() == 0);
}

/* An editable shader's Declaration is derived live from the edit buffer. */
static void test_assets_shader_declaration(void)
{
	asset_reset();
	assets_scheme_reset();
	script_eval("(set! kruddboard-assets-sel 801)");
	rec_reset();
	script_eval("(kruddboard-draw-assets)");

	assert(rec_has("text|format: krudd-shader"));
	assert(rec_has("text|stages: vertex"));
}

static void test_assets_shader_save_ok(void)
{
	asset_reset();
	assets_scheme_reset();
	script_eval("(set! kruddboard-assets-sel 801)");
	g_shader_save_ok = 1;
	g_click = "Save";
	rec_reset();
	script_eval("(kruddboard-draw-assets)");
	g_click = NULL;

	assert(rec_has("save-shader|801|"));
	assert(rec_has("colored|0.30,0.90,0.30,1.00|Compiled OK"));
}

/* A failed compile leaves the last-committed source live and shows the
 * failure text instead of silently dropping the edit. */
static void test_assets_shader_save_fail(void)
{
	asset_reset();
	assets_scheme_reset();
	script_eval("(set! kruddboard-assets-sel 801)");
	g_shader_save_ok = 0;
	g_click = "Save";
	rec_reset();
	script_eval("(kruddboard-draw-assets)");
	g_click = NULL;

	assert(rec_has("colored|1.00,0.30,0.30,1.00|Compile failed"));
}

/* A built-in shader gets the Clone flow, seeded "<path>_copy", instead of
 * Save/Delete. */
static void test_assets_shader_clone(void)
{
	asset_reset();
	assets_scheme_reset();
	script_eval("(set! kruddboard-assets-sel 601)");
	script_eval("(kruddboard-draw-assets)"); /* seed the clone name */
	g_click = "Clone";
	rec_reset();
	script_eval("(kruddboard-draw-assets)");
	g_click = NULL;

	assert(rec_has("input-enter|##clonename|builtin://shader/scene_copy"));
	assert(rec_has("clone-shader|builtin://shader/scene_copy|"));
	assert(assets_sel() != 601 && assets_sel() != 0);
}

/* A duplicate clone name reports the conflict and keeps the built-in
 * selected instead of navigating to a nonexistent new asset. */
static void test_assets_shader_clone_conflict(void)
{
	asset_reset();
	assets_scheme_reset();
	script_eval("(set! kruddboard-assets-sel 601)");
	script_eval("(kruddboard-draw-assets)");
	g_create_fail = 1;
	g_click = "Clone";
	rec_reset();
	script_eval("(kruddboard-draw-assets)");
	g_click = NULL;

	assert(rec_has("colored|1.00,0.30,0.30,1.00|"
		       "\"builtin://shader/scene_copy\" already exists"));
	assert(assets_sel() == 601);
}

/* An editable script's Declaration is derived live from the edit buffer. */
static void test_assets_script_declaration(void)
{
	asset_reset();
	assets_scheme_reset();
	script_eval("(set! kruddboard-assets-sel 851)");
	rec_reset();
	script_eval("(kruddboard-draw-assets)");

	assert(rec_has("text|format: krudd-script"));
	assert(rec_has("text|hooks: on-tick"));
	assert(rec_has("input-ml|##script|(script mine (on-tick (self t) 0))"));
}

static void test_assets_script_save_ok(void)
{
	asset_reset();
	assets_scheme_reset();
	script_eval("(set! kruddboard-assets-sel 851)");
	g_script_save_ok = 1;
	g_click = "Save";
	rec_reset();
	script_eval("(kruddboard-draw-assets)");
	g_click = NULL;

	assert(rec_has("save-script|851|"));
	assert(rec_has("colored|0.30,0.90,0.30,1.00|Saved"));
}

/* A rejected save (not a well-formed script) shows the failure text and leaves
 * the last-committed source live. */
static void test_assets_script_save_fail(void)
{
	asset_reset();
	assets_scheme_reset();
	script_eval("(set! kruddboard-assets-sel 851)");
	g_script_save_ok = 0;
	g_click = "Save";
	rec_reset();
	script_eval("(kruddboard-draw-assets)");
	g_click = NULL;

	assert(rec_has("colored|1.00,0.30,0.30,1.00|Not a valid script"));
}

/* A built-in script gets the Clone flow, seeded "<path>_copy", instead of
 * Save/Delete. */
static void test_assets_script_clone(void)
{
	asset_reset();
	assets_scheme_reset();
	script_eval("(set! kruddboard-assets-sel 651)");
	script_eval("(kruddboard-draw-assets)"); /* seed the clone name */
	g_click = "Clone";
	rec_reset();
	script_eval("(kruddboard-draw-assets)");
	g_click = NULL;

	assert(rec_has("input-enter|##clonename|builtin://script/spinner_copy"));
	assert(rec_has("clone-script|builtin://script/spinner_copy|"));
	assert(assets_sel() != 651 && assets_sel() != 0);
}

/* A duplicate clone name reports the conflict and keeps the built-in
 * selected. */
static void test_assets_script_clone_conflict(void)
{
	asset_reset();
	assets_scheme_reset();
	script_eval("(set! kruddboard-assets-sel 651)");
	script_eval("(kruddboard-draw-assets)");
	g_create_fail = 1;
	g_click = "Clone";
	rec_reset();
	script_eval("(kruddboard-draw-assets)");
	g_click = NULL;

	assert(rec_has("colored|1.00,0.30,0.30,1.00|"
		       "\"builtin://script/spinner_copy\" already exists"));
	assert(assets_sel() == 651);
}

/* Picking "Script" from the type combo dispatches to the script creator. */
static void test_assets_create_script(void)
{
	asset_reset();
	assets_scheme_reset();
	g_click = "New Asset";
	script_eval("(kruddboard-draw-assets)");
	g_click = "Create";
	g_input_id = "name"; g_input_text = "new.script"; g_input_commit = 0;
	g_combo_pick = 3; /* Script */
	rec_reset();
	script_eval("(kruddboard-draw-assets)");
	g_click = NULL; g_input_id = NULL; g_combo_pick = -1;

	assert(rec_has("create-script|new.script"));
}

static void test_assets_material_editor(void)
{
	asset_reset();
	assets_scheme_reset();
	script_eval("(set! kruddboard-assets-sel 901)");
	g_click = "Save";
	g_float_id = "##basecolor"; g_float_changed = 1;
	rec_reset();
	script_eval("(kruddboard-draw-assets)");
	g_click = NULL; g_float_id = NULL; g_float_changed = 0;

	assert(rec_has("coloredit|##basecolor"));
	assert(rec_has("save-material|901|"));
}

/* A read-only material gets the Clone flow, seeded "<path>_copy", instead
 * of Save/Delete. */
static void test_assets_material_clone(void)
{
	asset_reset();
	assets_scheme_reset();
	g_fa[4].read_only = 1;
	script_eval("(set! kruddboard-assets-sel 901)");
	script_eval("(kruddboard-draw-assets)"); /* seed the clone name */
	g_click = "Clone";
	rec_reset();
	script_eval("(kruddboard-draw-assets)");
	g_click = NULL;

	assert(rec_has("input-enter|##clonename|red.mat_copy"));
	assert(rec_has("clone-material|red.mat_copy|"));
	assert(assets_sel() != 901 && assets_sel() != 0);
}

/* A duplicate clone name reports the conflict and keeps the built-in
 * selected instead of navigating to a nonexistent new asset. */
static void test_assets_material_clone_conflict(void)
{
	asset_reset();
	assets_scheme_reset();
	g_fa[4].read_only = 1;
	script_eval("(set! kruddboard-assets-sel 901)");
	script_eval("(kruddboard-draw-assets)");
	g_create_fail = 1;
	g_click = "Clone";
	rec_reset();
	script_eval("(kruddboard-draw-assets)");
	g_click = NULL;

	assert(rec_has("colored|1.00,0.30,0.30,1.00|"
		       "\"red.mat_copy\" already exists"));
	assert(assets_sel() == 901);
}

/* Every other asset type (mesh, texture, font, scene) falls back to the
 * read-only Declaration + Catalog tables. */
static void test_assets_generic_fallback(void)
{
	asset_reset();
	assets_scheme_reset();
	script_eval("(set! kruddboard-assets-sel 501)");
	rec_reset();
	script_eval("(kruddboard-draw-assets)");

	assert(rec_has("text|Declaration"));
	assert(rec_has("table-begin|##decl|2"));
	assert(rec_has("text|path"));
	assert(rec_has("text|builtin://mesh/cube"));
	assert(rec_has("text|Catalog"));
	assert(rec_has("table-begin|##catalog|2"));
	assert(rec_has("text|Mesh"));
	assert(rec_has("text|Primitive"));
	assert(rec_has("text|yes")); /* read_only */
}

/* A stale selection (the asset was deleted elsewhere) returns to the
 * browser instead of drawing a broken inspector. */
static void test_assets_inspector_stale(void)
{
	asset_reset();
	assets_scheme_reset();
	script_eval("(set! kruddboard-assets-sel 9999)");
	rec_reset();
	script_eval("(kruddboard-draw-assets)");

	assert(assets_sel() == 0);
}

/* The New Asset form draws before the browser table, in that order. */
static void test_assets_composition(void)
{
	asset_reset();
	assets_scheme_reset();
	rec_reset();
	script_eval("(kruddboard-draw-assets)");

	assert(rec_index("header|Browser") >= 0);
	assert(rec_index("header|Browser") < rec_index("btn|New Asset"));
	assert(rec_index("btn|New Asset") < rec_index("table-begin|##assets|6"));
}

int main(void)
{
	setup();

	RUN(subsystems_table);
	RUN(subsystems_cells);
	RUN(subsystems_absent);
	RUN(log_all_levels_colored);
	RUN(log_filter_click);
	RUN(log_autoscroll_toggle);
	RUN(log_absent);
	RUN(krudd_composition);

	RUN(world_header);
	RUN(world_entity_list);
	RUN(world_no_entities);
	RUN(world_create);
	RUN(world_create_disabled);
	RUN(world_delete);
	RUN(world_select);
	RUN(world_inspector);
	RUN(world_inspector_empty);
	RUN(world_name_commit);
	RUN(world_name_uncommitted);
	RUN(world_transform_edit);
	RUN(world_mesh_bind);
	RUN(world_mesh_unbind);
	RUN(world_bind_disabled);
	RUN(world_script_bind);
	RUN(world_script_unbind);
	RUN(world_script_resolved);
	RUN(world_gizmo_chips);
	RUN(world_composition);

	RUN(assets_unavailable);
	RUN(assets_no_assets);
	RUN(assets_browser_table);
	RUN(assets_browser_table_show_builtin);
	RUN(assets_row_select);
	RUN(assets_new_asset_button);
	RUN(assets_new_asset_hidden_without_mut);
	RUN(assets_create_text);
	RUN(assets_create_shader);
	RUN(assets_new_asset_cancel);
	RUN(assets_text_editor);
	RUN(assets_text_save);
	RUN(assets_text_delete);
	RUN(assets_shader_declaration);
	RUN(assets_shader_save_ok);
	RUN(assets_shader_save_fail);
	RUN(assets_shader_clone);
	RUN(assets_shader_clone_conflict);
	RUN(assets_script_declaration);
	RUN(assets_script_save_ok);
	RUN(assets_script_save_fail);
	RUN(assets_script_clone);
	RUN(assets_script_clone_conflict);
	RUN(assets_create_script);
	RUN(assets_material_editor);
	RUN(assets_material_clone);
	RUN(assets_material_clone_conflict);
	RUN(assets_generic_fallback);
	RUN(assets_inspector_stale);
	RUN(assets_composition);

	printf("%d/%d kruddboard panel tests passed\n", tests_passed, tests_run);
	return tests_passed == tests_run ? 0 : 1;
}
