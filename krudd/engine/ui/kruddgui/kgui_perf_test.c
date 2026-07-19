/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Native test for the Scheme-authored perf HUD (kruddgui-perf-hud-draw in
 * kruddgui.scm) — the small top-right FPS + frame-time graph kruddgui.cpp
 * draws every tick regardless of editor chrome, so a hitch is visible in a
 * game's own play view. Like the other kgui_*_test harnesses it registers
 * *stub* kgui-* primitives that record their calls, loads the same image the
 * WASM host loads (KRUDDGUI_SCM), drives the panel with a steerable
 * krudd-stats, and asserts on the recorded draws plus the ring-buffer state
 * the graph reads. Pixel layout is left to browser verification.
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

static int rec_count(const char *needle)
{
	int i, n = 0;

	for (i = 0; i < g_rec_n; i++)
		if (strstr(g_rec[i], needle))
			n++;
	return n;
}

/* ------------------------------------------------------------------ */
/* Steerable krudd-stats                                               */
/* ------------------------------------------------------------------ */

static int   g_stats_live;
static float g_fps, g_frame_ms;

static void setup(void)
{
	s7_scheme *sc = script_s7();

	g_rec_n      = 0;
	g_stats_live = 1;
	g_fps        = 60.0f;
	g_frame_ms   = 16.7f;

	/* Reset the ring buffer a prior test's draws left dirty. */
	s7_eval_c_string(sc,
		"(set! kruddgui-perf-hist (make-vector kruddgui-perf-hist-n 0.0))");
	s7_eval_c_string(sc, "(set! kruddgui-perf-hist-i 0)");
}

/* ------------------------------------------------------------------ */
/* Stub primitives                                                     */
/* ------------------------------------------------------------------ */

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
	return s7_list(sc, 2, s7_make_real(sc, 400.0), s7_make_real(sc, 800.0));
}

/* (krudd-stats) -> (fps frame-ms frame-count), or #f when steered absent. */
static s7_pointer st_stats(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	if (!g_stats_live)
		return s7_f(sc);
	return s7_list(sc, 3, s7_make_real(sc, (double)g_fps),
		       s7_make_real(sc, (double)g_frame_ms),
		       s7_make_integer(sc, 100));
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
	def(sc, "kgui-panel-begin", st_panel_begin, 5);
	def(sc, "kgui-panel-end", st_nullary, 0);
	def(sc, "kgui-viewport-size", st_viewport, 0);
	def(sc, "krudd-stats", st_stats, 0);

	/* Accessors referenced elsewhere in the shared image but unused by the
	 * perf HUD; benign stubs so loading it doesn't fault. */
	def(sc, "krudd-gizmo-mode", st_stats, 0);
	def(sc, "krudd-set-gizmo-mode", st_nullary, 1);
	def(sc, "krudd-startup", st_nullary, 0);
	def(sc, "krudd-subsystems", st_nullary, 0);
	def(sc, "krudd-log-history", st_nullary, 0);

	assert(script_eval(KRUDDGUI_SCM) == 0);
	return sc;
}

static void draw(void)
{
	s7_scheme *sc = script_s7();
	s7_pointer fn = s7_name_to_value(sc, "kruddgui-perf-hud-draw");

	assert(s7_is_procedure(fn));
	s7_call(sc, fn, s7_nil(sc));
}

static int hist_n(void)
{
	return (int)s7_integer(
		s7_name_to_value(script_s7(), "kruddgui-perf-hist-n"));
}

static int hist_i(void)
{
	return (int)s7_integer(
		s7_name_to_value(script_s7(), "kruddgui-perf-hist-i"));
}

static double hist_ref(int i)
{
	char buf[64];

	snprintf(buf, sizeof(buf), "(vector-ref kruddgui-perf-hist %d)", i);
	return s7_number_to_real(script_s7(), s7_eval_c_string(script_s7(), buf));
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

static void test_no_stats_draws_nothing(void)
{
	g_stats_live = 0;
	draw();
	assert(g_rec_n == 0);
}

static void test_draws_fps_and_panel_region(void)
{
	g_fps = 59.4f;
	draw();
	assert(rec_has("panel kgui-perf"));
	assert(rec_has("text 59.4 fps"));
}

static void test_draws_background_rule_and_bars(void)
{
	draw();
	/* panel bg + track + rule + one bar per history slot. */
	assert(rec_count("rect") == 3 + hist_n());
}

static void test_history_pushes_and_wraps(void)
{
	int i, n = hist_n();

	for (i = 0; i < n + 5; i++) {
		g_frame_ms = (float)i;
		draw();
	}
	/* The write cursor wrapped exactly 5 slots past a full lap. */
	assert(hist_i() == 5);
	/* Slot 0 was overwritten by sample (n + 0), not left at sample 0. */
	assert(hist_ref(0) == (double)n);
	/* Slot 4 (hist-i - 1) is the most recent write: the last pushed sample. */
	assert(hist_ref(4) == (double)(n + 4));
	/* Slot 5..n-1 are untouched by the wrap: still their first-lap value. */
	assert(hist_ref(5) == 5.0);
	assert(hist_ref(n - 1) == (double)(n - 1));
}

static int scm_true(const char *expr)
{
	return s7_eval_c_string(script_s7(), expr) == s7_t(script_s7());
}

static void test_color_thresholds(void)
{
	/* Under the 60fps budget is green; the 30fps budget line is still warn,
	 * not bad, since the colour steps at "past", not "at". */
	assert(scm_true("(equal? (kruddgui-perf-color 10.0) kruddgui-perf-good)"));
	assert(scm_true("(equal? (kruddgui-perf-color 16.7) kruddgui-perf-good)"));
	assert(scm_true("(equal? (kruddgui-perf-color 20.0) kruddgui-perf-warn)"));
	assert(scm_true("(equal? (kruddgui-perf-color 33.3) kruddgui-perf-warn)"));
	assert(scm_true("(equal? (kruddgui-perf-color 50.0) kruddgui-perf-bad)"));
}

int main(void)
{
	setup_interp();

	RUN(no_stats_draws_nothing);
	RUN(draws_fps_and_panel_region);
	RUN(draws_background_rule_and_bars);
	RUN(history_pushes_and_wraps);
	RUN(color_thresholds);

	printf("\n%d/%d kgui perf tests passed\n", tests_passed, tests_run);
	return tests_passed == tests_run ? 0 : 1;
}
