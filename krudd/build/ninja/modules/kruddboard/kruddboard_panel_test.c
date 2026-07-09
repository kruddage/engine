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
	return s7_make_boolean(sc, clicked(l));
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

	printf("%d/%d kruddboard panel tests passed\n", tests_passed, tests_run);
	return tests_passed == tests_run ? 0 : 1;
}
