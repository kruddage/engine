/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Native test for the Scheme-authored widget foundations (the draggable slider
 * and 2D colour picker in kruddgui.scm, #492 PR6a). Like kgui_scene_test it
 * registers *stub* kgui-* primitives — here with a steerable kgui-region so a
 * test can simulate a captured pointer at a fraction across a named widget's
 * rect — loads the same image the WASM host loads (KRUDDGUI_SCM), drives the
 * Assets foundations panel, and asserts on the values the widgets write back and
 * the geometry they emit. It verifies the portable logic — the press-to-value
 * mapping and clamping, the HSV round-trip, and the one-picker-open discipline —
 * leaving pixel layout to browser verification.
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
/* Recorded draws                                                      */
/* ------------------------------------------------------------------ */

#define REC_MAX 512
#define REC_LEN 128

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

/*
 * A simulated captured pointer: kgui-region whose name contains g_region_id
 * returns pressed with its press point at (frac_x, frac_y) across the region's
 * own rect, so a test can say "held 75% across the metallic slider".
 */
static char  g_region_id[64];
static int   g_region_live;
static float g_region_fx, g_region_fy;

static void setup(void)
{
	s7_scheme *sc = script_s7();

	g_rec_n        = 0;
	g_tap_live     = 0;
	g_tap_x        = g_tap_y = 0.0f;
	g_region_id[0] = '\0';
	g_region_live  = 0;
	g_region_fx    = g_region_fy = 0.0f;

	/* Reset the overlay + demo state a previous test may have left set. */
	s7_eval_c_string(sc, "(set! kruddgui-assets-open #t)");
	s7_eval_c_string(sc, "(set! kruddgui-assets-scroll 0.0)");
	s7_eval_c_string(sc, "(set! kruddgui-assets-total 0.0)");
	s7_eval_c_string(sc, "(set! kruddgui-open-picker #f)");
	s7_eval_c_string(sc, "(set! kruddgui-fold-state '())");
	s7_eval_c_string(sc, "(set! kruddgui-demo-metallic 0.5)");
	s7_eval_c_string(sc, "(set! kruddgui-demo-rough 0.3)");
	s7_eval_c_string(sc, "(set! kruddgui-demo-color (list 0.95 0.55 0.15))");
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

static void def(s7_scheme *sc, const char *name, s7_function fn, int req)
{
	s7_define_function(sc, name, fn, req, 0, false, "stub");
}

static s7_scheme *setup_interp(void)
{
	s7_scheme *sc = script_s7();

	assert(sc);
	def(sc, "kgui-rect", st_rect, 8);
	def(sc, "kgui-text", st_text, 7);
	def(sc, "kgui-text-metrics", st_metrics, 1);
	def(sc, "kgui-panel-begin", st_nullary, 5);
	def(sc, "kgui-panel-end", st_nullary, 0);
	def(sc, "kgui-button", st_button, 4);
	def(sc, "kgui-clip", st_nullary, 4);
	def(sc, "kgui-clip-none", st_nullary, 0);
	def(sc, "kgui-region-drag", st_region_drag, 0);
	def(sc, "kgui-region-wheel", st_region_wheel, 0);
	def(sc, "kgui-region", st_region, 5);

	assert(script_eval(KRUDDGUI_SCM) == 0);
	return sc;
}

/* Draw the whole Assets foundations panel with the current steering. */
static void draw(void)
{
	s7_scheme *sc = script_s7();
	s7_pointer fn = s7_name_to_value(sc, "kruddgui-assets-draw-panel");

	assert(s7_is_procedure(fn));
	s7_call(sc, fn, s7_list(sc, 2, s7_make_real(sc, 400.0),
				s7_make_real(sc, 800.0)));
}

static double get_real(const char *name)
{
	return s7_real(s7_name_to_value(script_s7(), name));
}

static int close_to(double a, double b)
{
	return fabs(a - b) < 1e-3;
}

/* nth (0-based) real of a scheme list-valued global. */
static double get_nth(const char *name, int n)
{
	s7_scheme *sc = script_s7();

	return s7_real(s7_list_ref(sc, s7_name_to_value(sc, name), n));
}

static void tap(float x, float y)
{
	g_tap_x    = x;
	g_tap_y    = y;
	g_tap_live = 1;
}

static void press_region(const char *id, float fx, float fy)
{
	snprintf(g_region_id, sizeof(g_region_id), "%s", id);
	g_region_fx   = fx;
	g_region_fy   = fy;
	g_region_live = 1;
}

/* Evaluate a Scheme snippet against the loaded image and return its result. */
static s7_pointer eval(const char *src)
{
	return s7_eval_c_string(script_s7(), src);
}

static int is_true(s7_pointer p)
{
	return p == s7_t(script_s7());
}

/*
 * A layout cursor with a wide-open clip band, so a widget called directly is
 * always visible: origin x 10, width 380, running y 0.
 */
#define TEST_LAY "(kruddgui-lay 0.0 -1000.0 2000.0 10.0 380.0)"

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

static void test_panel_renders(void)
{
	draw();
	assert(rec_has("text ASSETS"));
	assert(rec_has("text Material"));   /* the fold header  */
	assert(rec_has("text Metallic"));   /* its body (open)  */
	assert(rec_has("text Base Color"));
	assert(rec_has("text Reset"));      /* the button row   */
	assert(rec_has("region demo-metallic"));
}

static void test_slider_maps_press_to_value(void)
{
	/* Held 75% across the metallic slider (range 0..1) -> 0.75. */
	press_region("demo-metallic", 0.75f, 0.5f);
	draw();
	assert(close_to(get_real("kruddgui-demo-metallic"), 0.75));
	/* The other slider, untouched, keeps its value. */
	assert(close_to(get_real("kruddgui-demo-rough"), 0.3));
}

static void test_slider_clamps_past_the_end(void)
{
	/* A press past the right edge clamps to the max, not beyond. */
	press_region("demo-rough", 1.5f, 0.5f);
	draw();
	assert(close_to(get_real("kruddgui-demo-rough"), 1.0));
}

/* The name of the currently open picker, or "" when none. */
static const char *open_picker(void)
{
	s7_pointer p = s7_name_to_value(script_s7(), "kruddgui-open-picker");

	return s7_is_string(p) ? s7_string(p) : "";
}

static void test_swatch_tap_toggles_picker(void)
{
	/*
	 * The swatch row sits below the label, a rule, the (open) Material fold
	 * header, two sliders and a rule:
	 * body-y 48 + 26 + 8 + 46 (fold) + 72 + 72 + 8 + 26 = 306, a 40px row. A
	 * tap on it opens the picker; nothing is open before, its regions draw.
	 */
	draw();
	assert(strcmp(open_picker(), "") == 0);
	assert(!rec_has("region demo-color-sv"));

	tap(200.0f, 326.0f);
	g_rec_n = 0;
	draw();
	assert(strcmp(open_picker(), "demo-color") == 0);
	assert(rec_has("region demo-color-sv"));
	assert(rec_has("region demo-color-hue"));

	/* A second tap on the swatch closes it again (one open at a time). */
	tap(200.0f, 326.0f);
	draw();
	assert(strcmp(open_picker(), "") == 0);
}

static void test_fold_collapses(void)
{
	/*
	 * The Material fold is open by default, so its body draws. Its header is
	 * the first row after the section label + rule: body-y 48 + 26 + 8 = 82,
	 * a 40px row. Tapping it collapses the fold — the body no longer draws.
	 */
	draw();
	assert(rec_has("text Material"));
	assert(rec_has("text Metallic"));

	tap(200.0f, 100.0f);
	g_rec_n = 0;
	draw();
	assert(rec_has("text Material"));       /* header still there */
	assert(!rec_has("text Metallic"));      /* body gated off     */
	assert(!rec_has("region demo-metallic"));
}

static void test_fold_toggle(void)
{
	/* A fold called directly: header rect is x10 y0 w380 h40 (see TEST_LAY). */
	const char *mk =
		"(kruddgui-fold " TEST_LAY " \"f1\" \"F1\" #f)";

	assert(!is_true(eval(mk)));             /* default closed        */
	tap(100.0f, 20.0f);
	assert(is_true(eval(mk)));              /* tap on header opens    */
	assert(is_true(eval(mk)));              /* stays open, no tap     */
	tap(100.0f, 20.0f);
	assert(!is_true(eval(mk)));             /* another tap closes     */

	/* A second fold is independent — still at its own default. */
	assert(!is_true(eval(
		"(kruddgui-fold " TEST_LAY " \"f2\" \"F2\" #f)")));
	assert(is_true(eval(
		"(kruddgui-fold " TEST_LAY " \"f3\" \"F3\" #t)")));
}

static void test_button_row_selects(void)
{
	/* cw = (380 - 6) / 2 = 187; left cell [10,197], right [203,390]. */
	const char *mk =
		"(kruddgui-button-row " TEST_LAY " (list \"Reset\" \"Warm\"))";
	s7_pointer r;

	assert(eval(mk) == s7_f(script_s7()));  /* no tap -> #f */

	tap(100.0f, 20.0f);
	r = eval(mk);
	assert(s7_is_string(r) && strcmp(s7_string(r), "Reset") == 0);

	tap(300.0f, 20.0f);
	r = eval(mk);
	assert(s7_is_string(r) && strcmp(s7_string(r), "Warm") == 0);
}

static void test_picker_hue_changes_colour(void)
{
	double r0, g0, b0, r1;

	s7_eval_c_string(script_s7(),
			 "(set! kruddgui-open-picker \"demo-color\")");
	r0 = get_nth("kruddgui-demo-color", 0);
	g0 = get_nth("kruddgui-demo-color", 1);
	b0 = get_nth("kruddgui-demo-color", 2);

	/* Drag the hue strip to its top (hue 0 -> pure-red family). */
	press_region("demo-color-hue", 0.5f, 0.0f);
	draw();
	r1 = get_nth("kruddgui-demo-color", 0);

	/* The colour moved, stays a valid triple, and reddened. */
	assert(!(close_to(get_nth("kruddgui-demo-color", 0), r0) &&
		 close_to(get_nth("kruddgui-demo-color", 1), g0) &&
		 close_to(get_nth("kruddgui-demo-color", 2), b0)));
	assert(r1 >= get_nth("kruddgui-demo-color", 1)); /* red is the max */
	assert(r1 >= get_nth("kruddgui-demo-color", 2));
}

static void test_picker_sv_corner_is_white(void)
{
	/* Top-left of the SV square is saturation 0, value 1 -> white. */
	s7_eval_c_string(script_s7(),
			 "(set! kruddgui-open-picker \"demo-color\")");
	press_region("demo-color-sv", 0.0f, 0.0f);
	draw();
	assert(close_to(get_nth("kruddgui-demo-color", 0), 1.0));
	assert(close_to(get_nth("kruddgui-demo-color", 1), 1.0));
	assert(close_to(get_nth("kruddgui-demo-color", 2), 1.0));
}

static void test_picker_sv_bottom_is_black(void)
{
	/* Bottom of the SV square is value 0 -> black, whatever the hue/sat. */
	s7_eval_c_string(script_s7(),
			 "(set! kruddgui-open-picker \"demo-color\")");
	press_region("demo-color-sv", 1.0f, 1.0f);
	draw();
	assert(close_to(get_nth("kruddgui-demo-color", 0), 0.0));
	assert(close_to(get_nth("kruddgui-demo-color", 1), 0.0));
	assert(close_to(get_nth("kruddgui-demo-color", 2), 0.0));
}

int main(void)
{
	setup_interp();

	RUN(panel_renders);
	RUN(slider_maps_press_to_value);
	RUN(slider_clamps_past_the_end);
	RUN(swatch_tap_toggles_picker);
	RUN(picker_hue_changes_colour);
	RUN(picker_sv_corner_is_white);
	RUN(picker_sv_bottom_is_black);
	RUN(fold_collapses);
	RUN(fold_toggle);
	RUN(button_row_selects);

	printf("\n%d/%d kgui widget tests passed\n", tests_passed, tests_run);
	return tests_passed == tests_run ? 0 : 1;
}
