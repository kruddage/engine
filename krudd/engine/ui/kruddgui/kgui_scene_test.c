/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Native test for the Scheme-authored Scene inspector (the World tab lifted onto
 * kruddgui in kruddgui.scm). The real kgui-* / krudd-entity-* primitives bind s7
 * to WebGL and the scene api, which only build for the browser — so this harness
 * registers *stub* primitives that record their calls (and let a test steer a
 * simulated tap or field commit), loads the same image the WASM host loads
 * (KRUDDGUI_SCM), drives the Scene procedures, and asserts on the recorded
 * mutations. It verifies the portable logic — list/drill navigation, entity
 * create/destroy/select, the name / transform / binding write-backs and the
 * param-menu save path — leaving pixel layout to browser verification.
 */
#include "script.h"

#include "s7.h"
#include "kruddgui_scm.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int tests_run;
static int tests_passed;

#define RUN(name) do {                 \
	tests_run++;                   \
	setup();                       \
	test_##name();                 \
	tests_passed++;                \
	printf("PASS: " #name "\n");   \
} while (0)

/* ------------------------------------------------------------------ */
/* Recorded calls                                                      */
/* ------------------------------------------------------------------ */

#define REC_MAX 512
#define REC_LEN 192

static char g_rec[REC_MAX][REC_LEN];
static int  g_rec_n;

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

static int rec_has(const char *needle)
{
	int i;

	for (i = 0; i < g_rec_n; i++)
		if (strstr(g_rec[i], needle))
			return 1;
	return 0;
}

/* ------------------------------------------------------------------ */
/* Steerable input                                                     */
/* ------------------------------------------------------------------ */

/* A simulated tap: kgui-button reports (and consumes) the first rect it hits. */
static float g_tap_x, g_tap_y;
static int   g_tap_live;

/* A simulated field commit: kgui-field with this id returns committed VALUE. */
static char g_field_id[64];
static char g_field_val[64];

/* Fixtures a test can flip. */
static int g_selected;
static int g_has_entities;
static int g_inspect_live;
static int g_has_material_param;

static void setup(void)
{
	g_rec_n              = 0;
	g_tap_live           = 0;
	g_tap_x = g_tap_y    = 0.0f;
	g_field_id[0]        = '\0';
	g_field_val[0]       = '\0';
	g_selected           = -1;
	g_has_entities       = 1;
	g_inspect_live       = 1;
	g_has_material_param = 0;

	/* Reset the overlay state the previous test may have left set. */
	s7_scheme *sc = script_s7();

	s7_eval_c_string(sc, "(set! kruddgui-scene-sel -1)");
	s7_eval_c_string(sc, "(set! kruddgui-scene-open #t)");
	s7_eval_c_string(sc, "(set! kruddgui-scene-open-combo #f)");
	s7_eval_c_string(sc, "(set! kruddgui-scene-scroll 0.0)");
	s7_eval_c_string(sc, "(set! kruddgui-scene-total 0.0)");
}

/* ------------------------------------------------------------------ */
/* Stub primitives                                                     */
/* ------------------------------------------------------------------ */

static double num(s7_scheme *sc, s7_pointer p)
{
	return s7_number_to_real(sc, p);
}

static s7_pointer st_rect(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	rec("rect");
	return s7_unspecified(sc);
}

static s7_pointer st_text(s7_scheme *sc, s7_pointer a)
{
	s7_pointer s = s7_list_ref(sc, a, 2);

	if (s7_is_string(s))
		rec("text %s", s7_string(s));
	return s7_unspecified(sc);
}

static s7_pointer st_metrics(s7_scheme *sc, s7_pointer a)
{
	s7_pointer s = s7_car(a);
	int        n = s7_is_string(s) ? (int)strlen(s7_string(s)) : 0;

	return s7_list(sc, 2, s7_make_real(sc, 8.0 * n), s7_make_real(sc, 12.0));
}

static s7_pointer st_panel_begin(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	return s7_unspecified(sc);
}

static s7_pointer st_nullary(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	return s7_unspecified(sc);
}

static s7_pointer st_clip(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	return s7_unspecified(sc);
}

static s7_pointer st_region_drag(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	return s7_list(sc, 2, s7_make_real(sc, 0.0), s7_make_real(sc, 0.0));
}

static s7_pointer st_region_wheel(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	return s7_make_real(sc, 0.0);
}

static s7_pointer st_viewport(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	return s7_list(sc, 2, s7_make_real(sc, 400.0), s7_make_real(sc, 800.0));
}

/* (kgui-button x y w h) -> #t if the live tap falls in the rect (once). */
static s7_pointer st_button(s7_scheme *sc, s7_pointer a)
{
	double x = num(sc, s7_car(a));
	double y = num(sc, s7_list_ref(sc, a, 1));
	double w = num(sc, s7_list_ref(sc, a, 2));
	double h = num(sc, s7_list_ref(sc, a, 3));

	if (g_tap_live && g_tap_x >= x && g_tap_x <= x + w &&
	    g_tap_y >= y && g_tap_y <= y + h) {
		g_tap_live = 0; /* consume: one tap, one button */
		return s7_t(sc);
	}
	return s7_f(sc);
}

/* (kgui-field id x y w h text mode) -> (display active? committed? caret-px). */
static s7_pointer st_field(s7_scheme *sc, s7_pointer a)
{
	s7_pointer id  = s7_car(a);
	s7_pointer txt = s7_list_ref(sc, a, 5);
	const char *sid = s7_is_string(id) ? s7_string(id) : "";
	const char *cur = s7_is_string(txt) ? s7_string(txt) : "";

	rec("field %s", sid);
	if (g_field_id[0] && strcmp(sid, g_field_id) == 0)
		return s7_list(sc, 4, s7_make_string(sc, g_field_val),
			       s7_f(sc), s7_t(sc), s7_make_real(sc, 0.0));
	return s7_list(sc, 4, s7_make_string(sc, cur), s7_f(sc), s7_f(sc),
		       s7_make_real(sc, 0.0));
}

/* ---- accessor stubs ---- */

static s7_pointer st_caps(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	return s7_list(sc, 2, s7_t(sc), s7_t(sc));
}

static s7_pointer st_selected(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	return s7_make_integer(sc, g_selected);
}

static s7_pointer st_entities(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	if (!g_has_entities)
		return s7_nil(sc);
	return s7_list(sc, 2,
		s7_cons(sc, s7_make_integer(sc, 0), s7_make_string(sc, "Cube")),
		s7_cons(sc, s7_make_integer(sc, 1), s7_f(sc)));
}

static s7_pointer st_inspect(s7_scheme *sc, s7_pointer a)
{
	float pos[3] = { 1.0f, 2.0f, 3.0f };
	float rot[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	float scl[3] = { 1.0f, 1.0f, 1.0f };
	s7_pointer p, r, s;

	(void)a;
	if (!g_inspect_live)
		return s7_f(sc);
	p = s7_list(sc, 3, s7_make_real(sc, pos[0]), s7_make_real(sc, pos[1]),
		    s7_make_real(sc, pos[2]));
	r = s7_list(sc, 4, s7_make_real(sc, rot[0]), s7_make_real(sc, rot[1]),
		    s7_make_real(sc, rot[2]), s7_make_real(sc, rot[3]));
	s = s7_list(sc, 3, s7_make_real(sc, scl[0]), s7_make_real(sc, scl[1]),
		    s7_make_real(sc, scl[2]));
	return s7_list(sc, 12, s7_make_string(sc, "Cube"), p, r, s, s7_f(sc),
		       s7_t(sc), s7_t(sc), s7_t(sc), s7_make_integer(sc, 10),
		       s7_make_integer(sc, 20), s7_f(sc), s7_make_integer(sc, 0));
}

static s7_pointer st_assets(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	return s7_list(sc, 1,
		s7_cons(sc, s7_make_integer(sc, 10), s7_make_string(sc, "mesh/box")));
}

static s7_pointer st_asset_find(s7_scheme *sc, s7_pointer a)
{
	int ref = (int)s7_integer(s7_car(a));

	if (ref == 10)
		return s7_make_string(sc, "mesh/box");
	if (ref == 20)
		return s7_make_string(sc, "material/default");
	return s7_f(sc);
}

static s7_pointer st_create(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	rec("entity-create");
	return s7_make_integer(sc, 7);
}

static s7_pointer st_destroy(s7_scheme *sc, s7_pointer a)
{
	rec("entity-destroy %d", (int)s7_integer(s7_car(a)));
	return s7_unspecified(sc);
}

static s7_pointer st_select(s7_scheme *sc, s7_pointer a)
{
	rec("entity-select %d", (int)s7_integer(s7_car(a)));
	return s7_unspecified(sc);
}

static s7_pointer st_set_name(s7_scheme *sc, s7_pointer a)
{
	s7_pointer s = s7_cadr(a);

	rec("set-name %d %s", (int)s7_integer(s7_car(a)),
	    s7_is_string(s) ? s7_string(s) : "?");
	return s7_unspecified(sc);
}

static s7_pointer st_set_transform(s7_scheme *sc, s7_pointer a)
{
	s7_pointer pos = s7_cadr(a);
	double     px  = num(sc, s7_car(pos));

	rec("set-transform %d px=%.3f", (int)s7_integer(s7_car(a)), px);
	return s7_unspecified(sc);
}

static s7_pointer st_set_render(s7_scheme *sc, s7_pointer a)
{
	rec("set-render-ref %d %d", (int)s7_integer(s7_car(a)),
	    (int)s7_integer(s7_cadr(a)));
	return s7_unspecified(sc);
}

static s7_pointer st_set_material(s7_scheme *sc, s7_pointer a)
{
	rec("set-material-ref %d %d", (int)s7_integer(s7_car(a)),
	    (int)s7_integer(s7_cadr(a)));
	return s7_unspecified(sc);
}

static s7_pointer st_set_script(s7_scheme *sc, s7_pointer a)
{
	rec("set-script-ref %d %d", (int)s7_integer(s7_car(a)),
	    (int)s7_integer(s7_cadr(a)));
	return s7_unspecified(sc);
}

static s7_pointer st_shader_ref(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	return s7_make_integer(sc, 30);
}

static s7_pointer st_material_texture(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	return s7_nil(sc); /* no texture slot */
}

static s7_pointer st_empty_params(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	return s7_nil(sc);
}

/* One float param ("scale") when g_has_material_param is set. */
static s7_pointer st_material_params(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	if (!g_has_material_param)
		return s7_nil(sc);
	return s7_list(sc, 1,
		s7_list(sc, 8, s7_make_string(sc, "scale"),
			s7_make_symbol(sc, "float"), s7_make_integer(sc, 0),
			s7_make_integer(sc, 4), s7_make_integer(sc, 1),
			s7_make_string(sc, "float"), s7_make_real(sc, 0.0),
			s7_make_real(sc, 1.0)));
}

static s7_pointer st_material_values(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	if (!g_has_material_param)
		return s7_nil(sc);
	return s7_list(sc, 1, s7_list(sc, 1, s7_make_real(sc, 2.0)));
}

static s7_pointer st_save_material(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	rec("save-material-params");
	return s7_unspecified(sc);
}

static s7_pointer st_save_generic(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	rec("save-params");
	return s7_unspecified(sc);
}

static void def(s7_scheme *sc, const char *name, s7_function fn, int req)
{
	s7_define_function(sc, name, fn, req, 0, false, "stub");
}

static s7_scheme *setup_interp(void)
{
	s7_scheme *sc = script_s7();

	assert(sc);
	/* Drawing / input primitives. */
	def(sc, "kgui-rect", st_rect, 8);
	def(sc, "kgui-text", st_text, 7);
	def(sc, "kgui-text-metrics", st_metrics, 1);
	def(sc, "kgui-panel-begin", st_panel_begin, 5);
	def(sc, "kgui-panel-end", st_nullary, 0);
	def(sc, "kgui-button", st_button, 4);
	def(sc, "kgui-field", st_field, 7);
	def(sc, "kgui-clip", st_clip, 4);
	def(sc, "kgui-clip-none", st_nullary, 0);
	def(sc, "kgui-region-drag", st_region_drag, 0);
	def(sc, "kgui-region-wheel", st_region_wheel, 0);
	def(sc, "kgui-region-pressed", st_nullary, 0);
	def(sc, "kgui-viewport-size", st_viewport, 0);

	/* Accessors the mode-bar / log / board consoles also read (unused here
	 * but referenced in the shared image; give them benign stubs). */
	def(sc, "krudd-gizmo-mode", st_selected, 0);
	def(sc, "krudd-set-gizmo-mode", st_nullary, 1);
	def(sc, "krudd-stats", st_nullary, 0);
	def(sc, "krudd-startup", st_nullary, 0);
	def(sc, "krudd-subsystems", st_nullary, 0);
	def(sc, "krudd-log-history", st_nullary, 0);

	/* Scene accessors. */
	def(sc, "krudd-world-caps", st_caps, 0);
	def(sc, "krudd-selected", st_selected, 0);
	def(sc, "krudd-world-entities", st_entities, 0);
	def(sc, "krudd-entity-inspect", st_inspect, 1);
	def(sc, "krudd-mesh-assets", st_assets, 0);
	def(sc, "krudd-material-assets", st_assets, 0);
	def(sc, "krudd-script-assets", st_assets, 0);
	def(sc, "krudd-asset-find", st_asset_find, 1);
	def(sc, "krudd-entity-create", st_create, 0);
	def(sc, "krudd-entity-destroy", st_destroy, 1);
	def(sc, "krudd-entity-select", st_select, 1);
	def(sc, "krudd-entity-set-name", st_set_name, 2);
	def(sc, "krudd-entity-set-transform", st_set_transform, 4);
	def(sc, "krudd-entity-set-render-ref", st_set_render, 2);
	def(sc, "krudd-entity-set-material-ref", st_set_material, 2);
	def(sc, "krudd-entity-set-script-ref", st_set_script, 2);

	/* Param accessors. */
	def(sc, "krudd-mesh-params", st_empty_params, 1);
	def(sc, "krudd-script-params", st_empty_params, 1);
	def(sc, "krudd-texture-params", st_empty_params, 1);
	def(sc, "krudd-asset-shader-ref", st_shader_ref, 1);
	def(sc, "krudd-material-texture", st_material_texture, 1);
	def(sc, "krudd-shader-material-params", st_material_params, 1);
	def(sc, "krudd-entity-material-values", st_material_values, 3);
	def(sc, "krudd-entity-mesh-values", st_empty_params, 2);
	def(sc, "krudd-entity-script-values", st_empty_params, 2);
	def(sc, "krudd-entity-texture-values", st_empty_params, 2);
	def(sc, "krudd-entity-save-material-params", st_save_material, 3);
	def(sc, "krudd-entity-save-mesh-params", st_save_generic, 3);
	def(sc, "krudd-entity-save-script-params", st_save_generic, 3);
	def(sc, "krudd-entity-save-texture-params", st_save_generic, 3);

	assert(script_eval(KRUDDGUI_SCM) == 0);
	return sc;
}

/* Draw the whole Scene panel with the current fixtures/steering. */
static void draw(void)
{
	s7_scheme *sc = script_s7();
	s7_pointer fn = s7_name_to_value(sc, "kruddgui-scene-draw-panel");

	assert(s7_is_procedure(fn));
	s7_call(sc, fn, s7_list(sc, 2, s7_make_real(sc, 400.0),
				s7_make_real(sc, 800.0)));
}

static void set_sel(int id)
{
	char buf[64];

	snprintf(buf, sizeof(buf), "(set! kruddgui-scene-sel %d)", id);
	s7_eval_c_string(script_s7(), buf);
}

static int get_sel(void)
{
	return (int)s7_integer(
		s7_name_to_value(script_s7(), "kruddgui-scene-sel"));
}

static void tap(float x, float y)
{
	g_tap_x    = x;
	g_tap_y    = y;
	g_tap_live = 1;
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

static void test_list_renders(void)
{
	set_sel(-1);
	draw();
	assert(rec_has("text + Entity"));
	assert(rec_has("text Cube"));
}

static void test_no_entities(void)
{
	g_has_entities = 0;
	set_sel(-1);
	draw();
	assert(rec_has("text (no entities)"));
}

static void test_create(void)
{
	set_sel(-1);
	/* "+ Entity" is the first body button, at (22, 48, 356, 40). */
	tap(200.0f, 68.0f);
	draw();
	assert(rec_has("entity-create"));
	assert(rec_has("entity-select 7"));
	assert(get_sel() == 7);
}

static void test_select_drills(void)
{
	set_sel(-1);
	/* Row 0 sits below the + Entity button: (22, 94, 356, 40). */
	tap(150.0f, 112.0f);
	draw();
	assert(rec_has("entity-select 0"));
	assert(get_sel() == 0);
}

static void test_delete_row(void)
{
	set_sel(-1);
	/* Row 0's x button: bx = 22+356-28-4 = 346, y = 94+6 = 100, 28x28. */
	tap(358.0f, 112.0f);
	draw();
	assert(rec_has("entity-destroy 0"));
}

static void test_inspector_renders(void)
{
	set_sel(0);
	draw();
	assert(rec_has("field ename"));
	assert(rec_has("text Position"));
	assert(rec_has("text Bindings"));
	assert(rec_has("field pos0"));
}

static void test_name_commit(void)
{
	set_sel(0);
	strcpy(g_field_id, "ename");
	strcpy(g_field_val, "Renamed");
	draw();
	assert(rec_has("set-name 0 Renamed"));
}

static void test_transform_commit(void)
{
	set_sel(0);
	strcpy(g_field_id, "pos0");
	strcpy(g_field_val, "5.0");
	draw();
	assert(rec_has("set-transform 0 px=5.000"));
}

static void test_stale_inspector_returns_to_list(void)
{
	set_sel(4);
	g_inspect_live = 0;
	draw();
	assert(get_sel() == -1);
}

static void test_back_button(void)
{
	set_sel(0);
	/* "< Back" header button: (18, ~5, 64, 26). */
	tap(40.0f, 18.0f);
	draw();
	assert(get_sel() == -1);
}

static void test_material_param_save(void)
{
	set_sel(0);
	g_has_material_param = 1;
	strcpy(g_field_id, "mp-00"); /* material param 0, component 0 */
	strcpy(g_field_val, "0.5");
	/* The param menu sits below the fold; measure once, then scroll it in. */
	draw();
	s7_eval_c_string(script_s7(), "(set! kruddgui-scene-scroll -10000.0)");
	draw();
	assert(rec_has("save-material-params"));
}

int main(void)
{
	setup_interp();

	RUN(list_renders);
	RUN(no_entities);
	RUN(create);
	RUN(select_drills);
	RUN(delete_row);
	RUN(inspector_renders);
	RUN(name_commit);
	RUN(transform_commit);
	RUN(stale_inspector_returns_to_list);
	RUN(back_button);
	RUN(material_param_save);

	printf("\n%d/%d kgui scene tests passed\n", tests_passed, tests_run);
	return tests_passed == tests_run ? 0 : 1;
}
