/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Native test for the Scheme-authored Assets browser (the Assets tab lifted onto
 * kruddgui in kruddgui.scm, #492 PR6b). The real kgui-* / krudd-asset-*
 * primitives bind s7 to WebGL and the asset catalog, which only build for the
 * browser — so this harness registers *stub* primitives that record their calls
 * (and let a test steer a simulated tap or fold state), loads the same image the
 * WASM host loads (KRUDDGUI_SCM), drives the Assets procedures, and asserts on the
 * recorded draws and the selection/create it drives. It verifies the portable
 * logic — the path-tree grouping, the package/folder folds, the leaf grid,
 * drill-in navigation and the New Asset dispatch — leaving pixel layout to browser
 * verification. Where a tap's target depends on layout the y is documented and
 * kept shallow; the tree/grouping is exercised geometry-free by direct call.
 */
#include "script.h"

#include "s7.h"
#include "kruddgui_scm.h"

#include <assert.h>
#include <math.h>
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
/* Recorded draws / calls                                              */
/* ------------------------------------------------------------------ */

#define REC_MAX 512
#define REC_LEN 160

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
/* Steerable input / fixtures                                          */
/* ------------------------------------------------------------------ */

/* A simulated tap: kgui-button reports (and consumes) the first rect it hits. */
static float g_tap_x, g_tap_y;
static int   g_tap_live;

/* A simulated field commit: kgui-field with this id returns committed VALUE. */
static char g_field_id[64];
static char g_field_val[64];

/*
 * A simulated captured pointer: kgui-region whose name contains g_region_id
 * returns pressed with its press point at (frac_x, frac_y) across the region's
 * own rect, so a test can steer a slider / picker drag geometry-free.
 */
static char  g_region_id[64];
static int   g_region_live;
static float g_region_fx, g_region_fy;

/* Fixtures a test can flip. */
static int g_has_api;      /* krudd-assets returns #f when 0 */
static int g_mut;          /* krudd-asset-mut? */
static int g_mat_texture;  /* krudd-material-texture binds a texture when set */
static int g_tex_bake_ok;  /* krudd-texture-bake returns 0 when clear */

static void setup(void)
{
	s7_scheme *sc = script_s7();

	g_rec_n        = 0;
	g_tap_live     = 0;
	g_tap_x = g_tap_y = 0.0f;
	g_field_id[0]  = '\0';
	g_field_val[0] = '\0';
	g_region_id[0] = '\0';
	g_region_live  = 0;
	g_region_fx = g_region_fy = 0.0f;
	g_has_api      = 1;
	g_mut          = 1;
	g_mat_texture  = 0;
	g_tex_bake_ok  = 1;

	/* Reset the overlay + browser state a prior test may have left set. */
	s7_eval_c_string(sc, "(set! kruddgui-assets-open #t)");
	s7_eval_c_string(sc, "(set! kruddgui-assets-sel 0)");
	s7_eval_c_string(sc, "(set! kruddgui-assets-scroll 0.0)");
	s7_eval_c_string(sc, "(set! kruddgui-assets-total 0.0)");
	s7_eval_c_string(sc, "(set! kruddgui-assets-naming #f)");
	s7_eval_c_string(sc, "(set! kruddgui-assets-new-name \"\")");
	s7_eval_c_string(sc, "(set! kruddgui-assets-new-type 0)");
	s7_eval_c_string(sc, "(set! kruddgui-fold-state '())");

	/* Reset the material + texture editors and shared combo/picker state. */
	s7_eval_c_string(sc, "(set! kruddgui-assets-mat-id 0)");
	s7_eval_c_string(sc, "(set! kruddgui-assets-tex-id 0)");
	s7_eval_c_string(sc, "(set! kruddgui-assets-clone-src 0)");
	s7_eval_c_string(sc, "(set! kruddgui-assets-clone-conflict #f)");
	s7_eval_c_string(sc, "(set! kruddgui-scene-open-combo #f)");
	s7_eval_c_string(sc, "(set! kruddgui-open-picker #f)");
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

static s7_pointer st_nullary(s7_scheme *sc, s7_pointer a)
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
	s7_pointer  id  = s7_car(a);
	s7_pointer  txt = s7_list_ref(sc, a, 5);
	const char *sid = s7_is_string(id) ? s7_string(id) : "";
	const char *cur = s7_is_string(txt) ? s7_string(txt) : "";

	if (g_field_id[0] && strcmp(sid, g_field_id) == 0)
		return s7_list(sc, 4, s7_make_string(sc, g_field_val), s7_f(sc),
			       s7_t(sc), s7_make_real(sc, 0.0));
	return s7_list(sc, 4, s7_make_string(sc, cur), s7_f(sc), s7_f(sc),
		       s7_make_real(sc, 0.0));
}

/* (kgui-region name x y w h) -> (pressed press-x press-y). */
static s7_pointer st_region(s7_scheme *sc, s7_pointer a)
{
	s7_pointer  nm = s7_car(a);
	const char *id = s7_is_string(nm) ? s7_string(nm) : "";
	double      x  = num(sc, s7_list_ref(sc, a, 1));
	double      y  = num(sc, s7_list_ref(sc, a, 2));
	double      w  = num(sc, s7_list_ref(sc, a, 3));
	double      h  = num(sc, s7_list_ref(sc, a, 4));

	rec("region %s", id);
	if (g_region_live && g_region_id[0] && strstr(id, g_region_id))
		return s7_list(sc, 3, s7_t(sc),
			       s7_make_real(sc, x + g_region_fx * w),
			       s7_make_real(sc, y + g_region_fy * h));
	return s7_list(sc, 3, s7_f(sc), s7_make_real(sc, 0.0),
		       s7_make_real(sc, 0.0));
}

/* ---- asset accessor stubs ---- */

static s7_pointer arow(s7_scheme *sc, int id, const char *path, int type,
		       int kind, int state, int size, int refs)
{
	return s7_list(sc, 7, s7_make_integer(sc, id), s7_make_string(sc, path),
		       s7_make_integer(sc, type), s7_make_integer(sc, kind),
		       s7_make_integer(sc, state), s7_make_integer(sc, size),
		       s7_make_integer(sc, refs));
}

/*
 * (krudd-assets) -> (engine-rows project-rows). The project group has a top-level
 * leaf ("hero") and one under a folder ("props/box"), so a test sees both the
 * leaf grid and a nested fold; the engine group has one built-in shader.
 */
static s7_pointer st_assets(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	if (!g_has_api)
		return s7_f(sc);
	return s7_list(sc, 2,
		s7_list(sc, 1,
			arow(sc, 1, "builtin://shader/scene-textured",
			     4, 1, 1, 0, 0)),
		s7_list(sc, 2,
			arow(sc, 10, "hero", 1, 0, 1, 128, 1),
			arow(sc, 11, "props/box", 1, 0, 1, 64, 0)));
}

static s7_pointer st_asset_mut(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	return s7_make_boolean(sc, g_mut);
}

static s7_pointer ainfo(s7_scheme *sc, const char *path, int type, int kind,
			int state, int size, int refs, int ro, int origin)
{
	return s7_list(sc, 8, s7_make_string(sc, path),
		       s7_make_integer(sc, type), s7_make_integer(sc, kind),
		       s7_make_integer(sc, state), s7_make_integer(sc, size),
		       s7_make_integer(sc, refs), s7_make_boolean(sc, ro),
		       s7_make_integer(sc, origin));
}

/* (krudd-asset-info id) -> info tuple, or #f for an unknown (stale) id. */
static s7_pointer st_asset_info(s7_scheme *sc, s7_pointer a)
{
	int id = (int)s7_integer(s7_car(a));

	if (id == 10)
		return ainfo(sc, "hero", 1, 0, 1, 128, 1, 0, 1);
	if (id == 11)
		return ainfo(sc, "props/box", 1, 0, 1, 64, 0, 0, 1);
	if (id == 1)
		return ainfo(sc, "builtin://shader/scene-textured",
			     4, 1, 1, 0, 0, 1, 0);
	/* An authored material (mutable) and a read-only built-in material. */
	if (id == 20)
		return ainfo(sc, "mat/brass", 3, 0, 1, 0, 0, 0, 1);
	if (id == 21)
		return ainfo(sc, "builtin://material/scene", 3, 1, 1, 0, 0, 1, 0);
	/* An authored (mutable) texture. */
	if (id == 25)
		return ainfo(sc, "tex/wood", 2, 0, 1, 0, 0, 0, 1);
	return s7_f(sc);
}

static const char *arg1_str(s7_scheme *sc, s7_pointer a)
{
	s7_pointer s = s7_car(a);

	return s7_is_string(s) ? s7_string(s) : "?";
}

static s7_pointer st_create_text(s7_scheme *sc, s7_pointer a)
{
	rec("create-text %s", arg1_str(sc, a));
	return s7_make_integer(sc, 42);
}

static s7_pointer st_create_shader(s7_scheme *sc, s7_pointer a)
{
	rec("create-shader %s", arg1_str(sc, a));
	return s7_make_integer(sc, 42);
}

static s7_pointer st_create_material(s7_scheme *sc, s7_pointer a)
{
	rec("create-material %s", arg1_str(sc, a));
	return s7_make_integer(sc, 42);
}

static s7_pointer st_create_script(s7_scheme *sc, s7_pointer a)
{
	rec("create-script %s", arg1_str(sc, a));
	return s7_make_integer(sc, 42);
}

static s7_pointer st_create_mesh(s7_scheme *sc, s7_pointer a)
{
	rec("create-mesh %s", arg1_str(sc, a));
	return s7_make_integer(sc, 42);
}

/* ---- material editor accessor stubs ---- */

/* (aid . path) list for the shader / texture combos. */
static s7_pointer st_shader_assets(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	return s7_list(sc, 1,
		s7_cons(sc, s7_make_integer(sc, 30),
			s7_make_string(sc, "shader/scene")));
}

static s7_pointer st_texture_assets(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	return s7_list(sc, 1,
		s7_cons(sc, s7_make_integer(sc, 40),
			s7_make_string(sc, "tex/wood")));
}

/* (krudd-asset-find ref) -> path for the combo preview labels. */
static s7_pointer st_asset_find(s7_scheme *sc, s7_pointer a)
{
	int ref = (int)s7_integer(s7_car(a));

	if (ref == 30)
		return s7_make_string(sc, "shader/scene");
	if (ref == 40)
		return s7_make_string(sc, "tex/wood");
	return s7_f(sc);
}

/* (krudd-asset-shader-ref id) -> the material's shader asset id. */
static s7_pointer st_shader_ref(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	return s7_make_integer(sc, 30);
}

/*
 * (krudd-material-texture id) -> (tex-ref w h) when a texture is bound (steered
 * by g_mat_texture), else () for the "(none)" binding.
 */
static s7_pointer st_material_texture(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	if (!g_mat_texture)
		return s7_nil(sc);
	return s7_list(sc, 3, s7_make_integer(sc, 40), s7_make_integer(sc, 512),
		       s7_make_integer(sc, 512));
}

/*
 * (krudd-shader-material-params shader-ref) -> two fields: a 4-component "color"
 * (routes to the swatch) and a 1-component "range" (routes to the slider), so a
 * test sees both interactive param kinds. Each is (name type off size comps kind
 * min max), the shape kruddgui-assets-param-widget reads.
 */
static s7_pointer st_material_params(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	return s7_list(sc, 2,
		s7_list(sc, 8, s7_make_string(sc, "tint"),
			s7_make_symbol(sc, "vec4"), s7_make_integer(sc, 0),
			s7_make_integer(sc, 16), s7_make_integer(sc, 4),
			s7_make_string(sc, "color"), s7_make_real(sc, 0.0),
			s7_make_real(sc, 0.0)),
		s7_list(sc, 8, s7_make_string(sc, "metal"),
			s7_make_symbol(sc, "float"), s7_make_integer(sc, 16),
			s7_make_integer(sc, 4), s7_make_integer(sc, 1),
			s7_make_string(sc, "range"), s7_make_real(sc, 0.0),
			s7_make_real(sc, 1.0)));
}

/* (krudd-material-values mat-id shader-ref) -> one value list per param. */
static s7_pointer st_material_values(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	return s7_list(sc, 2,
		s7_list(sc, 4, s7_make_real(sc, 0.9), s7_make_real(sc, 0.5),
			s7_make_real(sc, 0.1), s7_make_real(sc, 1.0)),
		s7_list(sc, 1, s7_make_real(sc, 0.5)));
}

static s7_pointer st_save_material(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	rec("save-material");
	return s7_unspecified(sc);
}

static s7_pointer st_clone_material(s7_scheme *sc, s7_pointer a)
{
	rec("clone-material %s", arg1_str(sc, a));
	return s7_make_integer(sc, 99);
}

static s7_pointer st_asset_delete(s7_scheme *sc, s7_pointer a)
{
	rec("asset-delete %d", (int)s7_integer(s7_car(a)));
	return s7_unspecified(sc);
}

/* ---- texture editor accessor stubs ---- */

/* (krudd-texture-params id) -> the same two-param shape the material uses. */
static s7_pointer st_texture_params(s7_scheme *sc, s7_pointer a)
{
	return st_material_params(sc, a);
}

/* (krudd-texture-values id) -> one value list per param (1-arg). */
static s7_pointer st_texture_values(s7_scheme *sc, s7_pointer a)
{
	return st_material_values(sc, a);
}

/* (krudd-texture-bake ref values res) -> a GL texture handle, or 0 when clear. */
static s7_pointer st_texture_bake(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	rec("texture-bake");
	return s7_make_integer(sc, g_tex_bake_ok ? 55 : 0);
}

/* (kgui-image x y w h tex [u0 v0 u1 v1] [r g b a]) -> unspecified. */
static s7_pointer st_image(s7_scheme *sc, s7_pointer a)
{
	rec("image tex=%d", (int)s7_integer(s7_list_ref(sc, a, 4)));
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
	def(sc, "kgui-panel-begin", st_nullary, 5);
	def(sc, "kgui-panel-end", st_nullary, 0);
	def(sc, "kgui-button", st_button, 4);
	def(sc, "kgui-field", st_field, 7);
	def(sc, "kgui-region", st_region, 5);
	def(sc, "kgui-clip", st_nullary, 4);
	def(sc, "kgui-clip-none", st_nullary, 0);
	def(sc, "kgui-region-drag", st_region_drag, 0);
	def(sc, "kgui-region-wheel", st_region_wheel, 0);

	/* Asset accessors the browser body reads. */
	def(sc, "krudd-assets", st_assets, 0);
	def(sc, "krudd-asset-mut?", st_asset_mut, 0);
	def(sc, "krudd-asset-info", st_asset_info, 1);
	def(sc, "krudd-asset-create-text", st_create_text, 1);
	def(sc, "krudd-asset-create-shader", st_create_shader, 1);
	def(sc, "krudd-asset-create-material", st_create_material, 1);
	def(sc, "krudd-asset-create-script", st_create_script, 1);
	def(sc, "krudd-asset-create-mesh", st_create_mesh, 1);

	/* Accessors the material editor reads / writes. */
	def(sc, "krudd-shader-assets", st_shader_assets, 0);
	def(sc, "krudd-texture-assets", st_texture_assets, 0);
	def(sc, "krudd-asset-find", st_asset_find, 1);
	def(sc, "krudd-asset-shader-ref", st_shader_ref, 1);
	def(sc, "krudd-material-texture", st_material_texture, 1);
	def(sc, "krudd-shader-material-params", st_material_params, 1);
	def(sc, "krudd-material-values", st_material_values, 2);
	/* Save / clone take (id/name shader values [tex-ref w h]) — 3 + 3. */
	s7_define_function(sc, "krudd-asset-save-material", st_save_material,
			   3, 3, false, "stub");
	s7_define_function(sc, "krudd-asset-clone-material", st_clone_material,
			   3, 3, false, "stub");
	def(sc, "krudd-asset-delete", st_asset_delete, 1);

	/* Accessors + primitive the texture editor reads / draws. */
	def(sc, "krudd-texture-params", st_texture_params, 1);
	def(sc, "krudd-texture-values", st_texture_values, 1);
	def(sc, "krudd-texture-bake", st_texture_bake, 3);
	s7_define_function(sc, "kgui-image", st_image, 5, 8, false, "stub");

	assert(script_eval(KRUDDGUI_SCM) == 0);
	return sc;
}

/* Draw the whole Assets panel with the current fixtures/steering. */
static void draw(void)
{
	s7_scheme *sc = script_s7();
	s7_pointer fn = s7_name_to_value(sc, "kruddgui-assets-draw-panel");

	assert(s7_is_procedure(fn));
	s7_call(sc, fn, s7_list(sc, 2, s7_make_real(sc, 400.0),
				s7_make_real(sc, 800.0)));
}

static void set_sel(int id)
{
	char buf[64];

	snprintf(buf, sizeof(buf), "(set! kruddgui-assets-sel %d)", id);
	s7_eval_c_string(script_s7(), buf);
}

static int get_sel(void)
{
	return (int)s7_integer(
		s7_name_to_value(script_s7(), "kruddgui-assets-sel"));
}

static int naming(void)
{
	return s7_name_to_value(script_s7(), "kruddgui-assets-naming")
		== s7_t(script_s7());
}

static void tap(float x, float y)
{
	g_tap_x    = x;
	g_tap_y    = y;
	g_tap_live = 1;
}

static s7_pointer eval(const char *src)
{
	return s7_eval_c_string(script_s7(), src);
}

static int close_to(double a, double b)
{
	return fabs(a - b) < 1e-3;
}

static int is_true(s7_pointer p)
{
	return p == s7_t(script_s7());
}

/* nth (0-based) real of a scheme list. */
static double nth_real(s7_pointer lst, int n)
{
	return s7_real(s7_list_ref(script_s7(), lst, n));
}

static void press_region(const char *id, float fx, float fy)
{
	snprintf(g_region_id, sizeof(g_region_id), "%s", id);
	g_region_fx   = fx;
	g_region_fy   = fy;
	g_region_live = 1;
}

/*
 * A layout cursor with a wide-open clip band, so a widget called directly is
 * always visible: origin x 10, width 380, running y 0 — the same geometry-free
 * harness kgui_widgets_test drives its widgets through.
 */
#define TEST_LAY "(kruddgui-lay 0.0 -1000.0 2000.0 10.0 380.0)"

/* The name of the currently open colour picker, or "" when none. */
static const char *open_picker(void)
{
	s7_pointer p = s7_name_to_value(script_s7(), "kruddgui-open-picker");

	return s7_is_string(p) ? s7_string(p) : "";
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

static void test_browser_renders(void)
{
	draw();
	assert(rec_has("text ASSETS"));        /* panel header       */
	assert(rec_has("+ New Asset"));        /* mutable -> the form control */
	assert(rec_has("krudd:engine"));       /* engine package fold header  */
	assert(rec_has("pkg:project"));        /* project package fold header */
	/* Project is open by default: its grid header + top-level leaf show. */
	assert(rec_has("text Name"));
	assert(rec_has("text hero"));
	assert(rec_has("text props"));         /* the folder fold header      */
	/* Engine is collapsed, and the props folder closed, so their leaves hide. */
	assert(!rec_has("scene-textured"));
	assert(!rec_has("text box"));
}

static void test_no_new_form_when_immutable(void)
{
	g_mut = 0;
	draw();
	assert(!rec_has("+ New Asset"));
	assert(rec_has("pkg:project"));
}

static void test_assets_unavailable(void)
{
	g_has_api = 0;
	draw();
	assert(rec_has("(assets unavailable)"));
}

static void test_folder_expands(void)
{
	/* Open the props folder fold directly (geometry-free): its leaf shows. */
	eval("(set! kruddgui-fold-state (list (cons \"project/props\" #t)))");
	draw();
	assert(rec_has("text box"));
}

static void test_select_leaf_via_tap(void)
{
	/*
	 * Immutable, so no New Asset form shifts the layout. Body starts at
	 * body-y 48; engine fold (closed) at 48, project fold (open) at 94, its
	 * grid header (a 26px line) at 140, the props folder fold at 166, then the
	 * "hero" leaf row at 212 — a 40px row. A tap on it drills in.
	 */
	g_mut = 0;
	tap(100.0f, 225.0f);
	draw();
	assert(get_sel() == 10);
}

static void test_inspector_renders(void)
{
	set_sel(10);
	draw();
	assert(rec_has("< Back"));
	assert(rec_has("text hero"));   /* the asset path        */
	assert(rec_has("text Type"));   /* a catalog field label */
	assert(rec_has("text Mesh"));   /* its value (type 1)    */
	assert(rec_has("text Read-only"));
}

static void test_inspector_back(void)
{
	set_sel(10);
	/* "< Back" is the first body row: (22, 48, 356, 40). */
	tap(100.0f, 60.0f);
	draw();
	assert(get_sel() == 0);
}

static void test_stale_inspector_returns_to_browser(void)
{
	set_sel(999);   /* krudd-asset-info returns #f for an unknown id */
	draw();
	assert(get_sel() == 0);
}

static void test_new_form_opens(void)
{
	/* "+ New Asset" is the first body row when mutable: (22, 48, 356, 40). */
	assert(!naming());
	tap(100.0f, 60.0f);
	draw();
	assert(naming());
}

static void test_create_dispatch(void)
{
	/* The type index picks the typed create primitive; 0 = Text is default. */
	assert((int)s7_integer(
		eval("(kruddgui-asset-create-of-type 2 \"mat1\")")) == 42);
	assert(rec_has("create-material mat1"));
	eval("(kruddgui-asset-create-of-type 0 \"notes\")");
	assert(rec_has("create-text notes"));
	eval("(kruddgui-asset-create-of-type 4 \"cube\")");
	assert(rec_has("create-mesh cube"));
}

static void test_path_segments(void)
{
	s7_scheme *sc = script_s7();
	s7_pointer r  =
		eval("(kruddgui-asset-path-segments "
		     "\"builtin://shader/scene-textured\")");

	assert(s7_list_length(sc, r) == 2);
	assert(strcmp(s7_string(s7_list_ref(sc, r, 0)), "shader") == 0);
	assert(strcmp(s7_string(s7_list_ref(sc, r, 1)), "scene-textured") == 0);

	/* A path with no "/" comes back as a single top-level leaf. */
	r = eval("(kruddgui-asset-path-segments \"hero\")");
	assert(s7_list_length(sc, r) == 1);
	assert(strcmp(s7_string(s7_list_ref(sc, r, 0)), "hero") == 0);
}

/* ------------------------------------------------------------------ */
/* Material editor (#492, PR6c)                                        */
/* ------------------------------------------------------------------ */

static void test_material_editor_renders(void)
{
	/* Drilling into a material (type 3) draws the editor, not the kv view. */
	set_sel(20);
	draw();
	assert(rec_has("< Back"));
	assert(rec_has("text Shader"));       /* the Shader fold / combo    */
	assert(rec_has("text Parameters"));   /* the Parameters fold        */
	assert(rec_has("text Texture"));      /* the Texture fold           */
	assert(rec_has("text tint"));         /* the colour param (swatch)  */
	assert(rec_has("text metal"));        /* the range param (slider)   */
	/* The slider (param index 1) declares its drag region; the swatch's
	 * regions only declare once its picker is open. */
	assert(rec_has("region mp-1"));
}

static void test_material_param_slider_routes(void)
{
	s7_pointer r;

	/* A "range" 1-comp field routes to the draggable slider; a press 75%
	 * across it maps to 0.75 and flags the change. */
	press_region("mp-0", 0.75f, 0.5f);
	r = eval("(kruddgui-assets-param-widget " TEST_LAY " \"mp-0\" "
		 "(list \"metal\" 'float 16 4 1 \"range\" 0.0 1.0) (list 0.5))");
	assert(close_to(nth_real(s7_car(r), 0), 0.75));
	assert(is_true(s7_cdr(r)));
	assert(rec_has("region mp-0"));
}

static void test_material_param_swatch_routes(void)
{
	/* A "color" field with >=3 comps routes to the colour swatch; a tap on
	 * its cell (label row 26 then swatch cell) opens the picker. */
	tap(100.0f, 40.0f);
	g_rec_n = 0;
	eval("(kruddgui-assets-param-widget " TEST_LAY " \"mp-0\" "
	     "(list \"tint\" 'vec4 0 16 4 \"color\" 0.0 0.0) "
	     "(list 0.9 0.5 0.1 1.0))");
	assert(strcmp(open_picker(), "mp-0") == 0);
	assert(rec_has("region mp-0-sv"));    /* the picker's regions declare */
	assert(rec_has("region mp-0-hue"));
}

static void test_material_param_numeric_routes(void)
{
	s7_pointer r;

	/* A field with neither hint routes to plain numeric fields; committing
	 * component 0 writes the parsed value back. */
	strcpy(g_field_id, "mp-00");
	strcpy(g_field_val, "1.5");
	r = eval("(kruddgui-assets-param-widget " TEST_LAY " \"mp-0\" "
		 "(list \"uv\" 'vec2 0 8 2 \"float\" 0.0 0.0) (list 0.0 0.0))");
	assert(close_to(nth_real(s7_car(r), 0), 1.5));
	assert(is_true(s7_cdr(r)));
}

/* Close the three editor folds so the action row sits at a known y. */
static void close_material_folds(void)
{
	eval("(set! kruddgui-fold-state "
	     "(list (cons \"mat-shader-fold\" #f) "
	     "(cons \"mat-params-fold\" #f) (cons \"mat-tex-fold\" #f)))");
}

static void test_material_save(void)
{
	/*
	 * Body at 48: Back (->94), path (->120), rule (->128), three closed folds
	 * (46 each: ->174 ->220 ->266), rule (->274), then the Save/Delete row at
	 * y 274. Save is the left half (x 22..197).
	 */
	set_sel(20);
	close_material_folds();
	tap(100.0f, 288.0f);
	draw();
	assert(rec_has("save-material"));
}

static void test_material_delete(void)
{
	/* Delete is the right half of the button-row (x 203..378, y 274..314). */
	set_sel(20);
	close_material_folds();
	tap(250.0f, 288.0f);
	draw();
	assert(rec_has("asset-delete 20"));
	assert(get_sel() == 0);
}

static void test_material_clone(void)
{
	/*
	 * A read-only material shows the Clone row instead: after the rule at 274,
	 * "Clone as" label (->300), the name field (46 ->346), then the Clone
	 * button at y 346..386.
	 */
	set_sel(21);
	close_material_folds();
	tap(100.0f, 360.0f);
	draw();
	assert(rec_has("clone-material"));
	assert(get_sel() == 99);
}

static void test_material_texture_resolution(void)
{
	/* A bound texture previews its path and reveals the Resolution combo. */
	g_mat_texture = 1;
	set_sel(20);
	draw();
	assert(rec_has("text tex/wood"));
	assert(rec_has("text Resolution"));
}

/* ------------------------------------------------------------------ */
/* Texture editor (#492, PR6e)                                         */
/* ------------------------------------------------------------------ */

static void test_texture_editor_renders(void)
{
	/* Drilling into a texture (type 2) draws the texture editor. */
	set_sel(25);
	draw();
	assert(rec_has("< Back"));
	assert(rec_has("text Declaration"));
	assert(rec_has("text Parameters"));
	assert(rec_has("text Preview"));
	assert(rec_has("text metal"));       /* a generation param (slider) */
	assert(rec_has("region tp-1"));      /* its drag region declares    */
	assert(rec_has("image tex=55"));     /* the live bake blits         */
}

static void test_texture_preview_unavailable(void)
{
	/* When the bake returns 0, a note stands in for the image. */
	g_tex_bake_ok = 0;
	set_sel(25);
	draw();
	assert(rec_has("(preview unavailable)"));
	assert(!rec_has("image tex="));
}

static void test_texture_delete(void)
{
	/*
	 * With the three folds closed the Delete row sits at a known y: Back
	 * (->94), path (->120), rule (->128), decl / params / preview folds (46
	 * each: ->174 ->220 ->266), rule (->274), Delete at y 274..314.
	 */
	set_sel(25);
	eval("(set! kruddgui-fold-state "
	     "(list (cons \"tex-decl-fold\" #f) "
	     "(cons \"tex-params-fold\" #f) (cons \"tex-preview-fold\" #f)))");
	tap(100.0f, 288.0f);
	draw();
	assert(rec_has("asset-delete 25"));
	assert(get_sel() == 0);
}

int main(void)
{
	setup_interp();

	RUN(browser_renders);
	RUN(no_new_form_when_immutable);
	RUN(assets_unavailable);
	RUN(folder_expands);
	RUN(select_leaf_via_tap);
	RUN(inspector_renders);
	RUN(inspector_back);
	RUN(stale_inspector_returns_to_browser);
	RUN(new_form_opens);
	RUN(create_dispatch);
	RUN(path_segments);
	RUN(material_editor_renders);
	RUN(material_param_slider_routes);
	RUN(material_param_swatch_routes);
	RUN(material_param_numeric_routes);
	RUN(material_save);
	RUN(material_delete);
	RUN(material_clone);
	RUN(material_texture_resolution);
	RUN(texture_editor_renders);
	RUN(texture_preview_unavailable);
	RUN(texture_delete);

	printf("\n%d/%d kgui assets tests passed\n", tests_passed, tests_run);
	return tests_passed == tests_run ? 0 : 1;
}
