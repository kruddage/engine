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

/*
 * A steerable kgui-field-multi: it echoes the field's text as the display and
 * reports the focus state / caret line / line count a test dials in, so the
 * multiline seam can be driven without the WASM edit buffer behind it.
 */
static int   g_fm_active, g_fm_committed, g_fm_cline, g_fm_nlines;
static float g_fm_caret_px;

/*
 * Steerable editor-toolbar accessors, standing in for the krudd-* bindings
 * kruddboard registers against the live edit/scene apis: the sim mode
 * (0 unsupported -> #f, 1 playing, 2 paused), the undo/redo availability, and
 * counters a tap test reads to prove the right thunk (or none) fired.
 */
static int g_sim, g_can_undo, g_can_redo;
static int g_undo_calls, g_redo_calls, g_toggle_calls;

static void setup(void)
{
	s7_scheme *sc = script_s7();

	g_rec_n        = 0;
	g_tap_live     = 0;
	g_tap_x        = g_tap_y = 0.0f;
	g_region_id[0] = '\0';
	g_region_live  = 0;
	g_region_fx    = g_region_fy = 0.0f;
	g_fm_active    = g_fm_committed = 0;
	g_fm_cline     = g_fm_nlines = 0;
	g_fm_caret_px  = 0.0f;
	g_sim          = 1;   /* playing, both histories empty by default */
	g_can_undo     = g_can_redo = 0;
	g_undo_calls   = g_redo_calls = g_toggle_calls = 0;

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

/* Records "text <str>"; when a size arg (8th) is present, appends "@<size>". */
static s7_pointer st_text(s7_scheme *sc, s7_pointer a)
{
	s7_pointer s = s7_list_ref(sc, a, 2);

	if (s7_is_string(s)) {
		int size = (s7_list_length(sc, a) > 7)
			 ? (int)num(sc, s7_list_ref(sc, a, 7)) : 0;

		if (size > 0)
			rec("text %s @%d", s7_string(s), size);
		else
			rec("text %s", s7_string(s));
	}
	return s7_unspecified(sc);
}

/*
 * 8px per character at the default (1-arg) size, keeping the existing widget
 * tests exact; with an explicit size the width scales as n*size*0.5 and the
 * reported height is that size, so the markdown wrap can be driven at any size.
 */
static s7_pointer st_metrics(s7_scheme *sc, s7_pointer a)
{
	s7_pointer s    = s7_car(a);
	s7_pointer rest = s7_cdr(a);
	int        n    = s7_is_string(s) ? (int)strlen(s7_string(s)) : 0;

	if (s7_is_pair(rest)) {
		double size = num(sc, s7_car(rest));

		return s7_list(sc, 2, s7_make_real(sc, n * size * 0.5),
			       s7_make_real(sc, size));
	}
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

/* (kgui-field-multi id x y w h text) -> the 6-tuple, echoing text as display. */
static s7_pointer st_field_multi(s7_scheme *sc, s7_pointer a)
{
	s7_pointer  t   = s7_list_ref(sc, a, 5);
	const char *txt = s7_is_string(t) ? s7_string(t) : "";

	rec("field-multi %s", g_fm_active ? "active" : "idle");
	return s7_list(sc, 6, s7_make_string(sc, txt),
		       s7_make_boolean(sc, g_fm_active),
		       s7_make_boolean(sc, g_fm_committed),
		       s7_make_real(sc, (double)g_fm_caret_px),
		       s7_make_integer(sc, g_fm_cline),
		       s7_make_integer(sc, g_fm_nlines));
}

/* (krudd-sim-mode) -> 'playing / 'paused / #f, as the harness has dialled it. */
static s7_pointer st_sim_mode(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	if (g_sim == 1)
		return s7_make_symbol(sc, "playing");
	if (g_sim == 2)
		return s7_make_symbol(sc, "paused");
	return s7_f(sc);
}

static s7_pointer st_can_undo(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	return s7_make_boolean(sc, g_can_undo);
}

static s7_pointer st_can_redo(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	return s7_make_boolean(sc, g_can_redo);
}

static s7_pointer st_undo(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	g_undo_calls++;
	return s7_unspecified(sc);
}

static s7_pointer st_redo(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	g_redo_calls++;
	return s7_unspecified(sc);
}

static s7_pointer st_toggle_sim(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	g_toggle_calls++;
	return s7_unspecified(sc);
}

static void def(s7_scheme *sc, const char *name, s7_function fn, int req)
{
	s7_define_function(sc, name, fn, req, 0, false, "stub");
}

/* Like def, but with opt optional args (for the size-taking text primitives). */
static void defo(s7_scheme *sc, const char *name, s7_function fn, int req,
		 int opt)
{
	s7_define_function(sc, name, fn, req, opt, false, "stub");
}

static s7_scheme *setup_interp(void)
{
	s7_scheme *sc = script_s7();

	assert(sc);
	def(sc, "kgui-rect", st_rect, 8);
	defo(sc, "kgui-text", st_text, 7, 1);
	defo(sc, "kgui-text-metrics", st_metrics, 1, 1);
	def(sc, "kgui-panel-begin", st_nullary, 5);
	def(sc, "kgui-panel-end", st_nullary, 0);
	def(sc, "kgui-button", st_button, 4);
	def(sc, "kgui-clip", st_nullary, 4);
	def(sc, "kgui-clip-none", st_nullary, 0);
	def(sc, "kgui-region", st_region, 5);
	def(sc, "kgui-field-multi", st_field_multi, 6);
	def(sc, "krudd-sim-mode", st_sim_mode, 0);
	def(sc, "krudd-can-undo", st_can_undo, 0);
	def(sc, "krudd-can-redo", st_can_redo, 0);
	def(sc, "krudd-undo", st_undo, 0);
	def(sc, "krudd-redo", st_redo, 0);
	def(sc, "krudd-toggle-sim", st_toggle_sim, 0);

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

/* nth (0-based) integer of a scheme list. */
static long nth_int(s7_pointer lst, int n)
{
	return (long)s7_integer(s7_list_ref(script_s7(), lst, n));
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

/* The line splitter behind the multiline field's per-line paint. */
static void test_multi_nth_line(void)
{
	assert(strcmp(s7_string(eval(
		"(kruddgui-scene-nth-line \"ab\\ncd\\nef\" 0)")), "ab") == 0);
	assert(strcmp(s7_string(eval(
		"(kruddgui-scene-nth-line \"ab\\ncd\\nef\" 1)")), "cd") == 0);
	assert(strcmp(s7_string(eval(
		"(kruddgui-scene-nth-line \"ab\\ncd\\nef\" 2)")), "ef") == 0);
	/* Past the last line is empty, and a trailing newline yields a blank line. */
	assert(strcmp(s7_string(eval(
		"(kruddgui-scene-nth-line \"ab\\ncd\" 9)")), "") == 0);
	assert(strcmp(s7_string(eval(
		"(kruddgui-scene-nth-line \"ab\\n\" 1)")), "") == 0);
}

/* Scroll-to-caret: the first visible line index keeps the caret in view. */
static void test_multi_top_math(void)
{
	/* Unfocused always shows the top. */
	assert(s7_integer(eval("(kruddgui-scene-multi-top 7 6 10 #f)")) == 0);
	/* Caret within the first page stays at top 0. */
	assert(s7_integer(eval("(kruddgui-scene-multi-top 3 6 10 #t)")) == 0);
	/* Caret past the bottom scrolls onto the last visible row. */
	assert(s7_integer(eval("(kruddgui-scene-multi-top 7 6 10 #t)")) == 2);
	assert(s7_integer(eval("(kruddgui-scene-multi-top 9 6 10 #t)")) == 4);
}

/* A 10-line focused field with the caret low: only the lines around it draw. */
static void test_multi_field_culls_to_caret(void)
{
	const char *ten =
		"\"L0\\nL1\\nL2\\nL3\\nL4\\nL5\\nL6\\nL7\\nL8\\nL9\"";
	char        src[256];
	s7_pointer  r;

	g_fm_active = 1;
	g_fm_cline  = 7;
	g_fm_nlines = 10;
	snprintf(src, sizeof(src),
		 "(kruddgui-scene-field-multi %s \"src\" %s)", TEST_LAY, ten);
	r = eval(src);

	/* rows = 6, caret line 7 -> top 2 -> visible lines 2..7 only. */
	assert(rec_has("text L2"));
	assert(rec_has("text L7"));
	assert(!rec_has("text L0"));
	assert(!rec_has("text L1"));
	assert(!rec_has("text L8"));
	assert(!rec_has("text L9"));

	/* The 6-tuple passes straight back to the caller. */
	assert(is_true(s7_list_ref(script_s7(), r, 1)));  /* active   */
	assert(nth_int(r, 4) == 7);                        /* caret-line */
	assert(nth_int(r, 5) == 10);                       /* nlines   */
}

/* An unfocused field shows its top page. */
static void test_multi_field_unfocused_shows_top(void)
{
	const char *ten =
		"\"L0\\nL1\\nL2\\nL3\\nL4\\nL5\\nL6\\nL7\\nL8\\nL9\"";
	char        src[256];

	g_fm_active = 0;
	g_fm_cline  = 0;
	g_fm_nlines = 10;
	snprintf(src, sizeof(src),
		 "(kruddgui-scene-field-multi %s \"src\" %s)", TEST_LAY, ten);
	eval(src);

	assert(rec_has("text L0"));
	assert(rec_has("text L5"));
	assert(!rec_has("text L6"));
}

/* ------------------------------------------------------------------ */
/* Editor toolbar (play/pause + undo/redo)                             */
/* ------------------------------------------------------------------ */

/* Read the label of the nth (0-based) descriptor in a toolbar button list. */
static const char *btn_label(s7_pointer btns, int n)
{
	return s7_string(s7_car(s7_list_ref(script_s7(), btns, n)));
}

/* Read the enabled flag (index 3) of the nth descriptor. */
static int btn_enabled(s7_pointer btns, int n)
{
	return is_true(s7_list_ref(script_s7(),
				   s7_list_ref(script_s7(), btns, n), 3));
}

/* Playing: the first chip is PAUSE, then UNDO/REDO greyed to their history. */
static void test_toolbar_playing_shows_pause(void)
{
	s7_pointer bs;

	g_sim = 1;
	g_can_undo = 1;
	g_can_redo = 0;
	bs = eval("(kruddgui-toolbar-buttons)");
	assert(s7_list_length(script_s7(), bs) == 3);
	assert(strcmp(btn_label(bs, 0), "PAUSE") == 0);
	assert(strcmp(btn_label(bs, 1), "UNDO") == 0);
	assert(btn_enabled(bs, 1));            /* history present */
	assert(strcmp(btn_label(bs, 2), "REDO") == 0);
	assert(!btn_enabled(bs, 2));           /* nothing to redo */
}

/* Paused: the first chip flips to PLAY. */
static void test_toolbar_paused_shows_play(void)
{
	s7_pointer bs;

	g_sim = 2;
	bs = eval("(kruddgui-toolbar-buttons)");
	assert(strcmp(btn_label(bs, 0), "PLAY") == 0);
}

/* No pausing support: the play/pause chip is omitted, only undo/redo remain. */
static void test_toolbar_no_sim_hides_playpause(void)
{
	s7_pointer bs;

	g_sim = 0;
	bs = eval("(kruddgui-toolbar-buttons)");
	assert(s7_list_length(script_s7(), bs) == 2);
	assert(strcmp(btn_label(bs, 0), "UNDO") == 0);
	assert(strcmp(btn_label(bs, 1), "REDO") == 0);
}

/*
 * A tap on the enabled play/pause chip fires the toggle. With n=3 chips of
 * w92 g10 the row is tot=296; centred in vw=800 that puts the first chip at
 * x0=252, y=margin(16), so its centre is (298, 38).
 */
static void test_toolbar_tap_toggles_sim(void)
{
	g_sim = 1;
	g_can_undo = 0;
	g_can_redo = 0;
	tap(298.0f, 38.0f);
	eval("(kruddgui-toolbar-draw 800.0 600.0)");
	assert(g_toggle_calls == 1);
}

/*
 * A tap on a *disabled* undo chip is swallowed by the toolbar region but runs
 * nothing, while the enabled redo beside it still fires. With no play/pause the
 * row is two chips (tot=194) centred in vw=800: UNDO at x0=303, REDO at 405.
 */
static void test_toolbar_disabled_undo_is_inert(void)
{
	g_sim = 0;
	g_can_undo = 0;   /* UNDO disabled */
	g_can_redo = 1;   /* REDO enabled  */
	tap(303.0f + 46.0f, 16.0f + 22.0f);
	eval("(kruddgui-toolbar-draw 800.0 600.0)");
	assert(g_undo_calls == 0);
}

static void test_toolbar_enabled_redo_fires(void)
{
	g_sim = 0;
	g_can_undo = 0;
	g_can_redo = 1;
	tap(405.0f + 46.0f, 16.0f + 22.0f);
	eval("(kruddgui-toolbar-draw 800.0 600.0)");
	assert(g_redo_calls == 1);
}

/* ------------------------------------------------------------------ */
/* Markdown preview (md_draw's kgui port)                              */
/* ------------------------------------------------------------------ */

/*
 * Greedy word wrap. At size 16 the stub metrics give 8px/char and an 8px space,
 * so "aaaa bbbb cccc" packs "aaaa bbbb" (72px) onto line 1 and breaks "cccc"
 * (would be 112px) onto line 2. Ranges are (start . end) over the source.
 */
static void test_md_wrap_breaks_lines(void)
{
	s7_pointer r, l0, l1;

	r = eval("(kruddgui-md-wrap \"aaaa bbbb cccc\" 80.0 16.0)");
	assert(s7_list_length(script_s7(), r) == 2);
	l0 = s7_car(r);
	l1 = s7_cadr(r);
	assert(s7_integer(s7_car(l0)) == 0 && s7_integer(s7_cdr(l0)) == 9);
	assert(s7_integer(s7_car(l1)) == 10 && s7_integer(s7_cdr(l1)) == 14);
}

/* A word wider than the whole width takes its own (overflowing) line. */
static void test_md_wrap_long_word_own_line(void)
{
	s7_pointer r;

	r = eval("(kruddgui-md-wrap \"tiny enormouswordhere\" 60.0 16.0)");
	assert(s7_list_length(script_s7(), r) == 2);
}

/* Runs flatten to one string plus (end . style) boundaries for colour lookup. */
static void test_md_runs_concat_and_style(void)
{
	eval("(define _tb (kruddgui-md-runs->text+bounds "
	     "(list (cons \"ab\" 0) (cons \"cd\" 2))))");
	assert(strcmp(s7_string(eval("(car _tb)")), "abcd") == 0);
	assert(s7_integer(eval("(kruddgui-md-style-at (cdr _tb) 0)")) == 0);
	assert(s7_integer(eval("(kruddgui-md-style-at (cdr _tb) 2)")) == 2);
	assert(s7_integer(eval("(kruddgui-md-seg-end (cdr _tb) 0)")) == 2);
}

/* A heading draws at its scaled size: h1 -> 32, h2 -> 24, h3 -> body 16. */
static void test_md_heading_scaled(void)
{
	eval("(kruddgui-md-block " TEST_LAY " (list 1 1 (list (cons \"Title\" 0))))");
	assert(rec_has("text Title @32"));
	g_rec_n = 0;
	eval("(kruddgui-md-block " TEST_LAY " (list 1 2 (list (cons \"Sub\" 0))))");
	assert(rec_has("text Sub @24"));
	g_rec_n = 0;
	eval("(kruddgui-md-block " TEST_LAY " (list 1 3 (list (cons \"Wee\" 0))))");
	assert(rec_has("text Wee @16"));
}

/* A code block lays a slab rect under its unwrapped, base-size line. */
static void test_md_code_has_slab(void)
{
	eval("(kruddgui-md-block " TEST_LAY " (list 3 0 (list (cons \"x=1\" 0))))");
	assert(rec_has("rect"));
	assert(rec_has("text x=1 @16"));
}

/*
 * A paragraph with a bold run draws the line as two colour segments — the plain
 * prefix and the styled word — split at the run boundary, not as one string.
 */
static void test_md_paragraph_styled_segments(void)
{
	eval("(kruddgui-md-block " TEST_LAY
	     " (list 0 0 (list (cons \"hi \" 0) (cons \"bold\" 1))))");
	assert(rec_has("text hi "));
	assert(rec_has("text bold"));
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
	RUN(multi_nth_line);
	RUN(multi_top_math);
	RUN(multi_field_culls_to_caret);
	RUN(multi_field_unfocused_shows_top);
	RUN(toolbar_playing_shows_pause);
	RUN(toolbar_paused_shows_play);
	RUN(toolbar_no_sim_hides_playpause);
	RUN(toolbar_tap_toggles_sim);
	RUN(toolbar_disabled_undo_is_inert);
	RUN(toolbar_enabled_redo_fires);
	RUN(md_wrap_breaks_lines);
	RUN(md_wrap_long_word_own_line);
	RUN(md_runs_concat_and_style);
	RUN(md_heading_scaled);
	RUN(md_code_has_slab);
	RUN(md_paragraph_styled_segments);

	printf("\n%d/%d kgui widget tests passed\n", tests_passed, tests_run);
	return tests_passed == tests_run ? 0 : 1;
}
