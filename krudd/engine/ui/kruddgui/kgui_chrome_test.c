/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Native test for the editor chrome in kruddgui.scm — core/editor_layout.scm's
 * spec, drawn kruddgui's way.
 *
 * Like kgui_scene_test it registers *stub* kgui-* primitives, loads the same
 * image the WASM host loads (KRUDDGUI_SCM), and drives the chrome through its
 * real per-tick entry point (kruddgui-chrome-tick) with a steerable tap. The
 * spec itself needs no fixture: script_init has already evaluated
 * core/editor_layout.scm into the shared interpreter, so what this drives is
 * the real tree, not a stand-in for it.
 *
 * What it checks is the half that is portable — that the chrome says what the
 * spec says, and that a gesture changes the state it should:
 *
 *   - every menu reaches the rail and every dock reaches the tray, with the
 *     spec's (tabbed-with …) collapsing two docks onto one pill and (raise …)
 *     choosing which of them that pill opens;
 *   - the View menu's rows are the dock list, because the reader expanded
 *     (dock-toggles) — the one place the menus and docks sections have to
 *     agree, checked here against a renderer rather than in isolation;
 *   - a tap does what the row says: hide a dock and its pill goes, hide the
 *     open one and the main area closes with it, reset and both come back;
 *   - an action the chrome has not wired lands in the status strip instead of
 *     doing nothing, which is the spec's own "coming soon" contract;
 *   - the spec's typography survives a 0x20..0x7E glyph atlas.
 *
 * Pixel layout is left to browser verification, exactly as the other kgui
 * tests leave it: the taps below are computed from the reserve order rather
 * than from a screenshot, so a band changing height moves them with it.
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
/* Recorded draws                                                      */
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

/* Exact-match count, for "how many pills did the tray draw". */
static int rec_count(const char *needle)
{
	int i, n = 0;

	for (i = 0; i < g_rec_n; i++)
		if (strstr(g_rec[i], needle))
			n++;
	return n;
}

/* ------------------------------------------------------------------ */
/* The viewport, and the geometry the taps are computed from           */
/* ------------------------------------------------------------------ */

/*
 * A portrait phone, the case the chrome is shaped for. Every tap below is
 * derived from these and from the order kruddgui-chrome-tick reserves its
 * bands in, so the numbers here are the only ones written down.
 */
#define VW 400.0f
#define VH 800.0f

#define MARGIN     16.0f  /* kruddgui-margin       */
#define GAP        10.0f  /* kruddgui-gap          */
#define RAIL_H     44.0f  /* kruddgui-chrome-rail-h */
#define BAND_H     28.0f  /* kruddgui-chrome-band-h */
#define TRAY_H     40.0f  /* kruddgui-tray-h        */
#define SWITCH_H   44.0f  /* kruddgui-modeswitch-h  */
#define ROW_H      44.0f  /* kruddgui-chrome-row-h  */
#define PAD        10.0f  /* kruddgui-chrome-pad    */
#define MENU_W     96.0f  /* kruddgui-chrome-menu-w */
#define PILL_W    104.0f  /* kruddgui-tray-pill-w   */
#define SHEET_W   280.0f  /* kruddgui-chrome-sheet-w */
#define TAB_H      32.0f  /* kruddgui-chrome-tab-h  */

/* The safe frame, then each band in the order the tick reserves it. */
#define FRAME_X (MARGIN)
#define FRAME_Y (MARGIN)
#define FRAME_W (VW - 2 * MARGIN)

#define RAIL_Y  (FRAME_Y)
#define BADGE_Y (RAIL_Y + RAIL_H + GAP)
#define TRAY_Y  (BADGE_Y + BAND_H + GAP)
#define MAIN_Y  (TRAY_Y + TRAY_H + GAP)

/*
 * The open sheet's left edge for menu INDEX: placed under its own rail pill,
 * then clamped so a sheet opened from the last menu still fits the frame. The
 * same clamp kruddgui-chrome-sheet-draw makes, restated here so a row tap is
 * derived rather than measured off a screenshot.
 */
static float sheet_x(int index)
{
	float under = FRAME_X + index * (MENU_W + GAP);
	float last  = FRAME_X + FRAME_W - SHEET_W;

	return under < last ? under : last;
}

/* The rail's pill pitch: four menus sharing the frame width. */
static float rail_pw(int n)
{
	float fit = (FRAME_W - (n - 1) * GAP) / n;

	return fit < MENU_W ? fit : MENU_W;
}

/* The tray's pill pitch: one per tab group. */
static float tray_pw(int n)
{
	float fit = (FRAME_W - (n - 1) * GAP) / n;

	return fit < PILL_W ? fit : PILL_W;
}

/* ------------------------------------------------------------------ */
/* Steerable input                                                     */
/* ------------------------------------------------------------------ */

static float g_tap_x, g_tap_y;
static int   g_tap_live;

static void tap(float x, float y)
{
	g_tap_x    = x;
	g_tap_y    = y;
	g_tap_live = 1;
}

/* ------------------------------------------------------------------ */
/* Stub primitives                                                     */
/* ------------------------------------------------------------------ */

static double num(s7_scheme *sc, s7_pointer v)
{
	return s7_number_to_real(sc, v);
}

static s7_pointer st_rect(s7_scheme *sc, s7_pointer a)
{
	rec("rect %.0f %.0f %.0f %.0f", num(sc, s7_car(a)),
	    num(sc, s7_list_ref(sc, a, 1)), num(sc, s7_list_ref(sc, a, 2)),
	    num(sc, s7_list_ref(sc, a, 3)));
	return s7_unspecified(sc);
}

static s7_pointer st_text(s7_scheme *sc, s7_pointer a)
{
	s7_pointer s = s7_list_ref(sc, a, 2);

	if (s7_is_string(s))
		rec("text %s", s7_string(s));
	return s7_unspecified(sc);
}

/*
 * A monospace stand-in for the SDF atlas: 8px per byte, 12px tall. The chrome
 * measures rather than counts, so the exact advance does not matter — only that
 * a longer run measures wider, which is what the wrap and the right-aligned
 * packing key off.
 */
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

static s7_pointer st_panel_begin(s7_scheme *sc, s7_pointer a)
{
	s7_pointer name = s7_car(a);

	if (s7_is_string(name))
		rec("panel %s", s7_string(name));
	return s7_unspecified(sc);
}

static s7_pointer st_viewport(s7_scheme *sc, s7_pointer a)
{
	(void)a;
	return s7_list(sc, 2, s7_make_real(sc, VW), s7_make_real(sc, VH));
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
	def(sc, "kgui-panel-begin", st_panel_begin, 5);
	def(sc, "kgui-panel-end", st_nullary, 0);
	def(sc, "kgui-button", st_button, 4);
	def(sc, "kgui-clip", st_nullary, 4);
	def(sc, "kgui-clip-none", st_nullary, 0);
	def(sc, "kgui-region-drag", st_region_drag, 0);
	def(sc, "kgui-region-wheel", st_region_wheel, 0);
	def(sc, "kgui-region-pressed", st_nullary, 0);
	def(sc, "kgui-viewport-size", st_viewport, 0);
	/*
	 * kgui-safe-insets is deliberately left unregistered: kruddgui-insets
	 * guards on (defined? …) and lays out against a plain margin without it,
	 * which is what the geometry above assumes.
	 */

	assert(script_eval(KRUDDGUI_SCM) == 0);
	return sc;
}

/* ------------------------------------------------------------------ */
/* Driving the chrome                                                  */
/* ------------------------------------------------------------------ */

static void ev(const char *expr)
{
	s7_eval_c_string(script_s7(), expr);
}

static int truthy(const char *expr)
{
	return s7_eval_c_string(script_s7(), expr) != s7_f(script_s7());
}

/* One tick of the chrome, through the same entry point kruddgui.cpp calls. */
static void draw(void)
{
	s7_scheme *sc = script_s7();
	s7_pointer fn = s7_name_to_value(sc, "kruddgui-chrome-tick");

	g_rec_n = 0;
	assert(s7_is_procedure(fn));
	s7_call(sc, fn, s7_nil(sc));
}

/* Reset every piece of chrome state between tests. */
static void setup(void)
{
	g_rec_n    = 0;
	g_tap_live = 0;
	ev("(set! kruddgui-chrome-menu #f)");
	ev("(set! kruddgui-chrome-open #f)");
	ev("(set! kruddgui-chrome-hidden '())");
	ev("(set! kruddgui-chrome-live '())");
	ev("(set! kruddgui-chrome-hint \"\")");
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

/*
 * Every menu the spec declares reaches the rail, mnemonic markers stripped —
 * the spec says "&File", the atlas has no underline, the pill says "File".
 */
static void test_rail_lists_menus(void)
{
	draw();
	assert(rec_has("panel kgui-chrome-rail"));
	assert(rec_has("text File"));
	assert(rec_has("text Edit"));
	assert(rec_has("text View"));
	assert(rec_has("text Help"));
	assert(!rec_has("text &File"));
}

/*
 * The tray collapses the spec's tab group: four docks, three pills, and the
 * group's pill is labelled by the dock the spec raises (Assets), not by the one
 * that tabs behind it (Console).
 */
static void test_tray_groups_docks(void)
{
	draw();
	assert(rec_has("panel kgui-chrome-tray"));
	assert(rec_has("text Scene"));
	assert(rec_has("text Inspector"));
	assert(rec_has("text Assets"));
	assert(!rec_has("text Console"));
	assert(truthy("(= (length (kruddgui-chrome-groups (editor-layout))) 3)"));
}

/* Tapping a rail pill opens that menu's sheet; tapping it again closes it. */
static void test_menu_opens_and_closes(void)
{
	float pw = rail_pw(4);

	draw(); /* place the rail before tapping it */
	tap(FRAME_X + pw / 2, RAIL_Y + RAIL_H / 2);
	draw();
	assert(truthy("(eqv? kruddgui-chrome-menu 0)"));

	tap(FRAME_X + pw / 2, RAIL_Y + RAIL_H / 2);
	draw();
	assert(truthy("(not kruddgui-chrome-menu)"));
}

/*
 * The File sheet draws the spec's own rows, with the shortcut column filled in
 * from the standard-key symbols the spec names.
 */
static void test_sheet_draws_rows(void)
{
	ev("(set! kruddgui-chrome-menu 0)");
	draw();
	assert(rec_has("panel kgui-chrome-sheet"));
	assert(rec_has("text New Project"));
	assert(rec_has("text Open Project..."));
	assert(rec_has("text Quit"));
	assert(rec_has("text ^N"));
	assert(rec_has("text ^Q"));
}

/*
 * The View sheet is the dock list, because the reader expanded (dock-toggles).
 * Nothing in this file names a dock — if the spec gains one, this row appears
 * with no edit here, which is the claim being tested.
 */
static void test_view_sheet_is_the_dock_list(void)
{
	ev("(set! kruddgui-chrome-menu 2)");
	draw();
	assert(rec_has("text Scene"));
	assert(rec_has("text Inspector"));
	assert(rec_has("text Assets"));
	assert(rec_has("text Console"));
	assert(rec_has("text Reset Layout"));
	/* Every dock starts visible, so every toggle carries the shown mark. */
	assert(rec_count("text ON") == 4);
}

/* Tapping a View toggle hides that dock: its row loses the mark, and the tray
 * loses its pill. */
static void test_toggle_hides_a_dock(void)
{
	ev("(set! kruddgui-chrome-menu 2)");
	draw();
	/* Row 0 of the View sheet is the first dock's toggle. */
	tap(sheet_x(2) + SHEET_W / 2, MAIN_Y + PAD + ROW_H / 2);
	draw();
	assert(truthy("(kruddgui-chrome-hidden? \"dock.scene\")"));

	/* The sheet closed with the tap, so this tick is the tray alone. */
	assert(!rec_has("text Scene"));
	assert(rec_has("text Inspector"));
	assert(truthy("(= (length (kruddgui-chrome-groups (editor-layout))) 2)"));
}

/* Reset Layout brings every hidden dock back and closes the main area. */
static void test_reset_layout(void)
{
	ev("(kruddgui-chrome-toggle-dock! \"dock.scene\")");
	ev("(kruddgui-chrome-toggle-dock! \"dock.inspector\")");
	assert(truthy("(= (length kruddgui-chrome-hidden) 2)"));

	ev("(kruddgui-chrome-act! \"reset-layout\")");
	assert(truthy("(null? kruddgui-chrome-hidden)"));
	assert(truthy("(not kruddgui-chrome-open)"));

	draw();
	assert(rec_has("text Scene"));
	assert(rec_has("text Inspector"));
}

/*
 * Tapping a group's tray pill opens it on the raised member, and the tab row
 * inside the card switches to the one that tabs behind it.
 */
static void test_group_pill_and_tabs(void)
{
	float pw = tray_pw(3);

	draw();
	tap(FRAME_X + 2 * (pw + GAP) + pw / 2, TRAY_Y + TRAY_H / 2);
	draw();
	assert(truthy("(string=? kruddgui-chrome-open \"dock.assets\")"));
	assert(rec_has("panel kgui-chrome-dock"));
	/* The card shows the raised member's heading from the spec. */
	assert(rec_has("text Asset Browser"));

	/* The tab row: two chips across the card, Console on the right. */
	tap(FRAME_X + FRAME_W * 0.75f, MAIN_Y + TAB_H / 2);
	draw();
	assert(truthy("(string=? kruddgui-chrome-open \"dock.console\")"));
	assert(rec_has("text Scheme REPL"));
}

/* Tapping the open group's pill again closes the main area. */
static void test_pill_toggles_closed(void)
{
	float pw = tray_pw(3);

	ev("(set! kruddgui-chrome-open \"dock.scene\")");
	draw();
	tap(FRAME_X + pw / 2, TRAY_Y + TRAY_H / 2);
	draw();
	assert(truthy("(not kruddgui-chrome-open)"));
	assert(!rec_has("panel kgui-chrome-dock"));
}

/*
 * Hiding the dock that is open closes the main area with it. Leaving it open
 * would strand a panel on screen with no pill left to dismiss it.
 */
static void test_hiding_the_open_dock_closes_it(void)
{
	ev("(set! kruddgui-chrome-open \"dock.scene\")");
	ev("(kruddgui-chrome-toggle-dock! \"dock.scene\")");
	assert(truthy("(not kruddgui-chrome-open)"));
}

/*
 * A (tabbed-with …) naming a dock that is hidden must not take its own dock
 * down with it: Console keeps a pill of its own rather than becoming
 * unreachable.
 */
static void test_hidden_tab_leader_leaves_a_pill(void)
{
	ev("(kruddgui-chrome-toggle-dock! \"dock.assets\")");
	draw();
	assert(truthy("(= (length (kruddgui-chrome-groups (editor-layout))) 3)"));
	assert(rec_has("text Console"));
}

/*
 * An action the chrome has not wired says so in the status strip. The spec is
 * explicit that which ids a host has wired is the host's business, so an
 * unknown id is a hint — never an error, and never a silent no-op.
 */
static void test_unwired_action_hints(void)
{
	ev("(kruddgui-chrome-act! \"save-as\")");
	assert(truthy("(> (string-length kruddgui-chrome-hint) 0)"));
	draw();
	assert(rec_has("save-as"));

	/* A wired one clears the hint rather than leaving the last one up. */
	ev("(kruddgui-chrome-act! \"reset-layout\")");
	assert(truthy("(= (string-length kruddgui-chrome-hint) 0)"));
}

/*
 * The toolbar band: the badge's seed text until the host writes over it by id,
 * and the version stamp packed against the trailing edge because the spec put a
 * (spacer) in front of it.
 */
static void test_badges(void)
{
	draw();
	assert(rec_has("panel kgui-chrome-badges"));
	assert(rec_has("text renderer - booting..."));
	assert(rec_has("text KRUDD Editor"));

	ev("(kruddgui-chrome-set! \"renderer\" \"WebGL2\")");
	ev("(kruddgui-chrome-set! \"version\" \"KRUDD 19.2.1\")");
	draw();
	assert(rec_has("text WebGL2"));
	assert(rec_has("text KRUDD 19.2.1"));
	assert(!rec_has("text renderer - booting..."));
}

/*
 * The status strip carries the spec's fields by id, live text included. The
 * resolution field's seed is two em dashes around a multiplication sign, which
 * is the case the fold exists for.
 */
static void test_status_fields(void)
{
	draw();
	assert(rec_has("panel kgui-chrome-status"));
	assert(rec_has("text fps -"));
	assert(rec_has("text -x-"));

	ev("(kruddgui-chrome-set! \"fps\" \"60 fps\")");
	ev("(kruddgui-chrome-set! \"driver\" \"WebGL2\")");
	draw();
	assert(rec_has("text 60 fps"));
	assert(rec_has("text WebGL2"));
}

/*
 * The typography fold, directly. The atlas is baked 0x20..0x7E, so the spec's
 * em dash, ellipsis and multiplication sign would each be a hole; folding them
 * keeps the spec written in prose and the chrome drawn in glyphs it has.
 */
static void test_ascii_fold(void)
{
	assert(truthy("(string=? (kruddgui-chrome-ascii \"a \xE2\x80\x94 b\")"
		      " \"a - b\")"));
	assert(truthy("(string=? (kruddgui-chrome-ascii \"more\xE2\x80\xA6\")"
		      " \"more...\")"));
	assert(truthy("(string=? (kruddgui-chrome-ascii \"800\xC3\x97""600\")"
		      " \"800x600\")"));
	/* Anything else non-ASCII is dropped whole, by its lead byte's
	 * continuation count, so the scan never desynchronizes. */
	assert(truthy("(string=? (kruddgui-chrome-ascii \"a\xE2\x82\xAC""b\")"
		      " \"ab\")"));
	assert(truthy("(string=? (kruddgui-chrome-ascii \"\") \"\")"));
	/* And the mnemonic strip rides along with it. */
	assert(truthy("(string=? (kruddgui-chrome-label \"&Save \xE2\x80\xA6\")"
		      " \"Save ...\")"));
}

/*
 * A tap that lands on the sheet's rows is claimed by the row; one that lands
 * outside the sheet dismisses it. Declaring the dismiss catcher last is what
 * makes the first half true, so this is the test that would fail if the three
 * kgui-button calls were ever reordered.
 */
static void test_tap_outside_dismisses_the_sheet(void)
{
	ev("(set! kruddgui-chrome-menu 0)");
	draw();
	/*
	 * Right edge of the free rect. The File sheet opens at the left and is
	 * SHEET_W wide, so this is outside its card — outside a row *and*
	 * outside the card's own swallow, which is what the dismiss needs.
	 */
	tap(FRAME_X + FRAME_W - 2.0f, MAIN_Y + 200.0f);
	draw();
	assert(truthy("(not kruddgui-chrome-menu)"));
}

/*
 * The chrome must degrade rather than trap when core's spec image is not in the
 * interpreter — the same guard kruddgui-insets makes for kgui-safe-insets, and
 * the reason a native image that loads only kruddgui.scm still runs.
 */
static void test_no_spec_is_a_no_op(void)
{
	ev("(define editor-layout-saved editor-layout)");
	ev("(set! editor-layout (lambda () #f))");
	draw();
	assert(!rec_has("panel kgui-chrome-rail"));
	ev("(set! editor-layout editor-layout-saved)");
	draw();
	assert(rec_has("panel kgui-chrome-rail"));
}

int main(void)
{
	setup_interp();

	RUN(rail_lists_menus);
	RUN(tray_groups_docks);
	RUN(menu_opens_and_closes);
	RUN(sheet_draws_rows);
	RUN(view_sheet_is_the_dock_list);
	RUN(toggle_hides_a_dock);
	RUN(reset_layout);
	RUN(group_pill_and_tabs);
	RUN(pill_toggles_closed);
	RUN(hiding_the_open_dock_closes_it);
	RUN(hidden_tab_leader_leaves_a_pill);
	RUN(unwired_action_hints);
	RUN(badges);
	RUN(status_fields);
	RUN(ascii_fold);
	RUN(tap_outside_dismisses_the_sheet);
	RUN(no_spec_is_a_no_op);

	printf("\n%d/%d kgui chrome tests passed\n", tests_passed, tests_run);
	return tests_passed == tests_run ? 0 : 1;
}
