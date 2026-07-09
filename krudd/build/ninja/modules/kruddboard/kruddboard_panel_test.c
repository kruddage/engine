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
	rec("header|%s", s7_string(s7_car(a)));
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
	int      has_render, has_material;
	unsigned render_ref, material_ref;
};

static struct fw_entity g_fw[FW_MAX];
static int              g_fw_count;
static int              g_fw_sel;
static int              g_have_entity;
static int              g_have_asset;
static int              g_gizmo;

/* type: 1 = ASSET_TYPE_MESH, 3 = ASSET_TYPE_MATERIAL (per asset_api.h). */
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
	g_assets_n = 3;

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
	return s7_list(sc, 10,
		s7_make_string(sc, e->name),
		real_vec(sc, e->pos, 3),
		real_vec(sc, e->rot, 4),
		real_vec(sc, e->scl, 3),
		parent,
		s7_make_boolean(sc, e->has_name),
		s7_make_boolean(sc, e->has_render),
		s7_make_boolean(sc, e->has_material),
		s7_make_integer(sc, e->has_render ? (int)e->render_ref : 0),
		s7_make_integer(sc, e->has_material ? (int)e->material_ref : 0));
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
	def(sc, "imgui-collapsing-header", st_collapsing_header, 1);
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
	def(sc, "krudd-asset-find", st_asset_find, 1);
	def(sc, "krudd-entity-create", st_entity_create, 0);
	def(sc, "krudd-entity-destroy", st_entity_destroy, 1);
	def(sc, "krudd-entity-select", st_entity_select, 1);
	def(sc, "krudd-entity-set-name", st_entity_set_name, 2);
	def(sc, "krudd-entity-set-transform", st_entity_set_transform, 4);
	def(sc, "krudd-entity-set-render-ref", st_entity_set_render_ref, 2);
	def(sc, "krudd-entity-set-material-ref", st_entity_set_material_ref, 2);
	def(sc, "krudd-gizmo-mode", st_gizmo_mode, 0);
	def(sc, "krudd-set-gizmo-mode", st_set_gizmo_mode, 1);

	assert(script_eval(KRUDDBOARD_SCM) == 0);
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

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

/* The subsystems table lays out header + one row per subsystem, in order. */
static void test_subsystems_table(void)
{
	reset_state();
	rec_reset();
	draw("kruddboard-draw-subsystems");

	assert(rec_has("table-begin|##subsys|4"));
	assert(rec_has("col|Name") && rec_has("col|WASM Size"));
	assert(rec_has("headers"));
	assert(rec_count("row") == 3);
	assert(rec_has("text|log"));
	assert(rec_has("text|memory"));
	assert(rec_has("text|kruddboard"));
	assert(rec_has("table-end"));
}

/* yes/- for API and Tick; the numeric size when set, a dimmed "-" when zero. */
static void test_subsystems_cells(void)
{
	reset_state();
	rec_reset();
	draw("kruddboard-draw-subsystems");

	assert(rec_has("text|yes"));      /* an API/Tick that is present */
	assert(rec_has("text|-"));        /* an API/Tick that is absent   */
	assert(rec_has("text|1024"));     /* a known WASM size            */
	assert(rec_count("disabled|-") == 2); /* two zero sizes, dimmed   */
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
	assert(rec_has("table-begin|##subsys|4")); /* subsystems drawn */
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
	RUN(world_gizmo_chips);
	RUN(world_composition);

	printf("%d/%d kruddboard panel tests passed\n", tests_passed, tests_run);
	return tests_passed == tests_run ? 0 : 1;
}
