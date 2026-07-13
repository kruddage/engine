/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Native test for the Scheme-authored widget foundations and shared layout
 * vocabulary (the draggable slider, the 2D colour picker, the fold header and
 * the button row in kruddgui.scm, #492 PR6a/6b). Like kgui_scene_test it
 * registers *stub* kgui-* primitives — here with a steerable kgui-region so a
 * test can simulate a captured pointer at a fraction across a named widget's
 * rect, and a steerable tap — loads the same image the WASM host loads
 * (KRUDDGUI_SCM), and drives each widget *directly*: it builds a layout cursor
 * with a wide-open clip band and calls the widget, then reads the value it
 * returns and the state it writes. Driving the helpers directly (rather than
 * through a panel body) keeps the tests geometry-free, so inserting a row above a
 * widget can't shift a hard-coded y out from under it. It verifies the portable
 * logic — the press-to-value mapping and clamping, the HSV round-trip, the
 * one-picker-open discipline, and the fold's independent open state — leaving
 * pixel layout to browser verification.
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

	/* Reset the one-open-at-a-time picker and the fold set a prior test set. */
	s7_eval_c_string(sc, "(set! kruddgui-open-picker #f)");
	s7_eval_c_string(sc, "(set! kruddgui-fold-state '())");
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
	def(sc, "kgui-region", st_region, 5);

	assert(script_eval(KRUDDGUI_SCM) == 0);
	return sc;
}

static int close_to(double a, double b)
{
	return fabs(a - b) < 1e-3;
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

/* nth (0-based) real of a scheme list. */
static double nth_real(s7_pointer lst, int n)
{
	return s7_real(s7_list_ref(script_s7(), lst, n));
}

/*
 * A layout cursor with a wide-open clip band, so a widget called directly is
 * always visible: origin x 10, width 380, running y 0.
 */
#define TEST_LAY "(kruddgui-lay 0.0 -1000.0 2000.0 10.0 380.0)"

/* The name of the currently open picker, or "" when none. */
static const char *open_picker(void)
{
	s7_pointer p = s7_name_to_value(script_s7(), "kruddgui-open-picker");

	return s7_is_string(p) ? s7_string(p) : "";
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

static void test_slider_maps_press_to_value(void)
{
	s7_pointer r;

	/* Held 75% across a slider over range 0..1 -> 0.75, and flagged changed. */
	press_region("s1", 0.75f, 0.5f);
	r = eval("(kruddgui-slider " TEST_LAY " \"s1\" \"S1\" 0.5 0.0 1.0)");
	assert(close_to(s7_real(s7_car(r)), 0.75));
	assert(is_true(s7_cdr(r)));
}

static void test_slider_clamps_past_the_end(void)
{
	s7_pointer r;

	/* A press past the right edge clamps to the max, not beyond. */
	press_region("s1", 1.5f, 0.5f);
	r = eval("(kruddgui-slider " TEST_LAY " \"s1\" \"S1\" 0.5 0.0 1.0)");
	assert(close_to(s7_real(s7_car(r)), 1.0));
}

static void test_slider_untouched_holds_value(void)
{
	s7_pointer r;

	/* A press on a *different* widget leaves this slider unchanged. */
	press_region("other", 0.9f, 0.5f);
	r = eval("(kruddgui-slider " TEST_LAY " \"s1\" \"S1\" 0.3 0.0 1.0)");
	assert(close_to(s7_real(s7_car(r)), 0.3));
	assert(!is_true(s7_cdr(r)));
}

static void test_swatch_tap_toggles_picker(void)
{
	/*
	 * The swatch draws a label line first (26px) then the swatch cell, so its
	 * tappable rect is x10 y26 w380 h40 in TEST_LAY; a tap on it toggles the
	 * picker.
	 */
	const char *mk =
		"(kruddgui-color-swatch " TEST_LAY " \"c1\" \"C1\" "
		"(list 0.9 0.5 0.1))";

	assert(strcmp(open_picker(), "") == 0);
	assert(!rec_has("region c1-sv"));

	tap(100.0f, 40.0f);
	g_rec_n = 0;
	eval(mk);
	assert(strcmp(open_picker(), "c1") == 0);
	assert(rec_has("region c1-sv"));   /* the picker's regions now declare */
	assert(rec_has("region c1-hue"));

	/* A second tap on the swatch closes it again (one open at a time). */
	tap(100.0f, 40.0f);
	eval(mk);
	assert(strcmp(open_picker(), "") == 0);
}

static void test_picker_hue_changes_colour(void)
{
	s7_pointer r, rgb;

	eval("(set! kruddgui-open-picker \"c1\")");
	/* Drag the hue strip to its top (hue 0 -> the pure-red family). */
	press_region("c1-hue", 0.5f, 0.0f);
	r = eval("(kruddgui-color-swatch " TEST_LAY " \"c1\" \"C1\" "
		 "(list 0.2 0.5 0.9))");
	rgb = s7_car(r);
	/* Red is now the dominant channel, and the swatch flags the change. */
	assert(nth_real(rgb, 0) >= nth_real(rgb, 1));
	assert(nth_real(rgb, 0) >= nth_real(rgb, 2));
	assert(is_true(s7_cdr(r)));
}

static void test_picker_sv_corner_is_white(void)
{
	s7_pointer rgb;

	/* Top-left of the SV square is saturation 0, value 1 -> white. */
	eval("(set! kruddgui-open-picker \"c1\")");
	press_region("c1-sv", 0.0f, 0.0f);
	rgb = s7_car(eval("(kruddgui-color-swatch " TEST_LAY " \"c1\" \"C1\" "
			  "(list 0.2 0.5 0.9))"));
	assert(close_to(nth_real(rgb, 0), 1.0));
	assert(close_to(nth_real(rgb, 1), 1.0));
	assert(close_to(nth_real(rgb, 2), 1.0));
}

static void test_picker_sv_bottom_is_black(void)
{
	s7_pointer rgb;

	/* Bottom of the SV square is value 0 -> black, whatever the hue/sat. */
	eval("(set! kruddgui-open-picker \"c1\")");
	press_region("c1-sv", 1.0f, 1.0f);
	rgb = s7_car(eval("(kruddgui-color-swatch " TEST_LAY " \"c1\" \"C1\" "
			  "(list 0.2 0.5 0.9))"));
	assert(close_to(nth_real(rgb, 0), 0.0));
	assert(close_to(nth_real(rgb, 1), 0.0));
	assert(close_to(nth_real(rgb, 2), 0.0));
}

static void test_swatch_preserves_alpha(void)
{
	s7_pointer rgb;

	/* A 4th (alpha) component survives an edit through the picker. */
	eval("(set! kruddgui-open-picker \"c1\")");
	press_region("c1-sv", 0.0f, 0.0f);
	rgb = s7_car(eval("(kruddgui-color-swatch " TEST_LAY " \"c1\" \"C1\" "
			  "(list 0.2 0.5 0.9 0.25))"));
	assert(s7_list_length(script_s7(), rgb) == 4);
	assert(close_to(nth_real(rgb, 3), 0.25));
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

int main(void)
{
	setup_interp();

	RUN(slider_maps_press_to_value);
	RUN(slider_clamps_past_the_end);
	RUN(slider_untouched_holds_value);
	RUN(swatch_tap_toggles_picker);
	RUN(picker_hue_changes_colour);
	RUN(picker_sv_corner_is_white);
	RUN(picker_sv_bottom_is_black);
	RUN(swatch_preserves_alpha);
	RUN(fold_toggle);
	RUN(button_row_selects);

	printf("\n%d/%d kgui widget tests passed\n", tests_passed, tests_run);
	return tests_passed == tests_run ? 0 : 1;
}
