/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Native test for the Scheme-authored GAME / EDITOR slider
 * (kruddgui-modeswitch-draw in kruddgui.scm) — the two-position switch
 * kruddgui.cpp draws every tick in both modes, so a player who booted into a
 * game can reach the editor and back. Like the other kgui_*_test harnesses it
 * registers *stub* kgui-* primitives that record their calls, loads the same
 * image the WASM host loads (KRUDDGUI_SCM), and drives the panel with a
 * steerable editor-mode flag and a steerable tap. It asserts on the placement
 * the rect helper computes, on which half the accent knob lands, and on what
 * a tap asks the host for; pixel appearance is left to browser verification.
 */
#include <core/script.h>

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

#define REC_MAX 64
#define REC_LEN 128

static char g_rec[REC_MAX][REC_LEN];
static int  g_rec_n;

/* The rects the draw emitted, in order, so a test can find the knob. */
struct rec_rect {
	double x, y, w, h;
};

static struct rec_rect g_rects[REC_MAX];
static int             g_rects_n;

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
/* Steerable host state                                                */
/* ------------------------------------------------------------------ */

/* The viewport the stub reports, and the safe insets under it. */
static double g_vw, g_vh;
static double g_inset_bottom;

/*
 * plugin_abi.c's editor-mode flag, stood in for here: what the panel reads
 * through kgui-editor-mode, plus a count of the writes kgui-set-editor-mode
 * took so a tap test can prove which mode was asked for (and that a tap on
 * the lit half asked for nothing at all).
 */
static int g_editor_mode;
static int g_set_calls;
static int g_set_last;

/* A simulated tap: kgui-button reports (and consumes) the first rect it hits. */
static double g_tap_x, g_tap_y;
static int    g_tap_live;

static void setup(void)
{
	g_rec_n        = 0;
	g_rects_n      = 0;
	g_vw           = 400.0;
	g_vh           = 800.0;
	g_inset_bottom = 0.0;
	g_editor_mode  = 0;
	g_set_calls    = 0;
	g_set_last     = -1;
	g_tap_live     = 0;
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
	if (g_rects_n < REC_MAX) {
		g_rects[g_rects_n].x = num(sc, s7_car(a));
		g_rects[g_rects_n].y = num(sc, s7_list_ref(sc, a, 1));
		g_rects[g_rects_n].w = num(sc, s7_list_ref(sc, a, 2));
		g_rects[g_rects_n].h = num(sc, s7_list_ref(sc, a, 3));
		g_rects_n++;
	}
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

/* A fixed-metric font: 8px per character on a 12px line. */
static s7_pointer st_metrics(s7_scheme *sc, s7_pointer a)
{
	s7_pointer  s = s7_car(a);
	const char *t = s7_is_string(s) ? s7_string(s) : "";

	return s7_list(sc, 2, s7_make_real(sc, 8.0 * (double)strlen(t)),
		       s7_make_real(sc, 12.0));
}

static s7_pointer st_panel_begin(s7_scheme *sc, s7_pointer a)
{
	s7_pointer nm = s7_car(a);

	if (s7_is_string(nm))
		rec("panel %s", s7_string(nm));
	return s7_unspecified(sc);
}

static s7_pointer st_nullary(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	return s7_unspecified(sc);
}

static s7_pointer st_viewport(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	return s7_list(sc, 2, s7_make_real(sc, g_vw), s7_make_real(sc, g_vh));
}

/* (kgui-safe-insets) -> (top right bottom left). */
static s7_pointer st_insets(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	return s7_list(sc, 4, s7_make_real(sc, 0.0), s7_make_real(sc, 0.0),
		       s7_make_real(sc, g_inset_bottom),
		       s7_make_real(sc, 0.0));
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

static s7_pointer st_editor_mode(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	return s7_make_boolean(sc, g_editor_mode != 0);
}

static s7_pointer st_set_editor_mode(s7_scheme *sc, s7_pointer a)
{
	g_editor_mode = s7_boolean(sc, s7_car(a)) ? 1 : 0;
	g_set_last    = g_editor_mode;
	g_set_calls++;
	return s7_nil(sc);
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
	s7_define_function(sc, "kgui-text-metrics", st_metrics, 1, 1, false,
			   "stub");
	def(sc, "kgui-panel-begin", st_panel_begin, 5);
	def(sc, "kgui-panel-end", st_nullary, 0);
	def(sc, "kgui-viewport-size", st_viewport, 0);
	def(sc, "kgui-safe-insets", st_insets, 0);
	def(sc, "kgui-button", st_button, 4);
	def(sc, "kgui-editor-mode", st_editor_mode, 0);
	def(sc, "kgui-set-editor-mode", st_set_editor_mode, 1);

	assert(script_eval(KRUDDGUI_SCM) == 0);
	return sc;
}

/* ------------------------------------------------------------------ */
/* Driving the panel                                                   */
/* ------------------------------------------------------------------ */

static void draw(void)
{
	s7_scheme *sc = script_s7();
	s7_pointer fn = s7_name_to_value(sc, "kruddgui-modeswitch-draw");

	assert(s7_is_procedure(fn));
	s7_call(sc, fn, s7_nil(sc));
}

static void tap(double x, double y)
{
	g_tap_x    = x;
	g_tap_y    = y;
	g_tap_live = 1;
}

static int close_to(double a, double b)
{
	return fabs(a - b) < 1e-3;
}

/* The slider's own rect, as the Scheme helper computes it. */
static void switch_rect(double *x, double *y, double *w, double *h)
{
	s7_scheme *sc = script_s7();
	char       buf[96];
	s7_pointer r;

	snprintf(buf, sizeof(buf), "(kruddgui-modeswitch-rect %g %g)",
		 g_vw, g_vh);
	r  = s7_eval_c_string(sc, buf);
	*x = num(sc, s7_car(r));
	*y = num(sc, s7_list_ref(sc, r, 1));
	*w = num(sc, s7_list_ref(sc, r, 2));
	*h = num(sc, s7_list_ref(sc, r, 3));
}

/*
 * The accent knob: the third rect the draw emits (backing frame, then the
 * track, then the lit half) — the labels that follow draw no rects.
 */
static struct rec_rect knob(void)
{
	assert(g_rects_n >= 3);
	return g_rects[2];
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

static void test_draws_both_halves_in_one_region(void)
{
	draw();
	assert(rec_has("panel kgui-modeswitch"));
	assert(rec_has("text GAME"));
	assert(rec_has("text EDITOR"));
}

static void test_knob_sits_on_the_game_half(void)
{
	double x, y, w, h;

	switch_rect(&x, &y, &w, &h);
	draw();
	assert(close_to(knob().x, x));
	assert(close_to(knob().w, w / 2.0));
}

static void test_knob_slides_to_the_editor_half(void)
{
	double x, y, w, h;

	g_editor_mode = 1;
	switch_rect(&x, &y, &w, &h);
	draw();
	assert(close_to(knob().x, x + w / 2.0));
}

static void test_tap_on_editor_half_enters_editor(void)
{
	double x, y, w, h;

	switch_rect(&x, &y, &w, &h);
	tap(x + w * 0.75, y + h / 2.0);
	draw();
	assert(g_set_calls == 1);
	assert(g_set_last == 1);
}

static void test_tap_on_game_half_returns_to_game(void)
{
	double x, y, w, h;

	g_editor_mode = 1;
	switch_rect(&x, &y, &w, &h);
	tap(x + w * 0.25, y + h / 2.0);
	draw();
	assert(g_set_calls == 1);
	assert(g_set_last == 0);
}

/* Tapping the half already lit re-asks for the mode it is in, never toggles. */
static void test_tap_on_the_lit_half_holds_the_mode(void)
{
	double x, y, w, h;

	switch_rect(&x, &y, &w, &h);
	tap(x + w * 0.25, y + h / 2.0);
	draw();
	assert(g_editor_mode == 0);
	assert(g_set_last != 1);
}

/* A tap outside the strip is the game's, not the switch's. */
static void test_tap_above_the_switch_is_not_taken(void)
{
	double x, y, w, h;

	switch_rect(&x, &y, &w, &h);
	tap(x + w / 2.0, y - 100.0);
	draw();
	assert(g_set_calls == 0);
}

static void test_clears_the_bottom_safe_inset(void)
{
	double x, y, w, h;

	g_inset_bottom = 34.0;
	switch_rect(&x, &y, &w, &h);
	assert(close_to(y + h, g_vh - g_inset_bottom - 16.0));
}

static void test_clamps_to_a_narrow_viewport(void)
{
	double x, y, w, h;

	g_vw = 200.0;
	switch_rect(&x, &y, &w, &h);
	assert(close_to(w, 200.0 - 32.0));
	assert(close_to(x, 16.0));
}

static void test_centres_on_a_wide_viewport(void)
{
	double x, y, w, h;

	g_vw = 1600.0;
	switch_rect(&x, &y, &w, &h);
	assert(close_to(w, 220.0));
	assert(close_to(x, (1600.0 - 220.0) / 2.0));
}

/* A viewport the host has not sized yet draws nothing at all. */
static void test_zero_viewport_draws_nothing(void)
{
	g_vw = 0.0;
	g_vh = 0.0;
	draw();
	assert(g_rec_n == 0);
}

int main(void)
{
	setup_interp();

	RUN(draws_both_halves_in_one_region);
	RUN(knob_sits_on_the_game_half);
	RUN(knob_slides_to_the_editor_half);
	RUN(tap_on_editor_half_enters_editor);
	RUN(tap_on_game_half_returns_to_game);
	RUN(tap_on_the_lit_half_holds_the_mode);
	RUN(tap_above_the_switch_is_not_taken);
	RUN(clears_the_bottom_safe_inset);
	RUN(clamps_to_a_narrow_viewport);
	RUN(centres_on_a_wide_viewport);
	RUN(zero_viewport_draws_nothing);

	printf("\n%d/%d kgui mode-switch tests passed\n", tests_passed,
	       tests_run);
	return tests_passed == tests_run ? 0 : 1;
}
