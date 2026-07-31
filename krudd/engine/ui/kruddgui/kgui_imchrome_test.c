/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Native test for the editor chrome in kruddgui.scm — core/editor_layout.scm's
 * spec, drawn the ImGui way.
 *
 * Like kgui_scene_test it registers *stub* kgui-* primitives, loads the same
 * image the WASM host loads (KRUDDGUI_SCM), and drives the chrome through its
 * real per-tick entry point (kruddgui-ig-tick) with a steerable click. The spec
 * itself needs no fixture: script_init has already evaluated
 * core/editor_layout.scm into the shared interpreter, so what this drives is
 * the real tree, not a stand-in for it.
 *
 * What it checks is the half that is portable — that the chrome says what the
 * spec says, and that a click changes the state it should:
 *
 *   - every menu reaches the menu bar and every dock reaches the dockspace, in
 *     the area the spec declares it for, all of them on screen at once;
 *   - the spec's (tabbed-with …) becomes a tab bar and (raise) picks which tab
 *     opens forward, with a click on the other tab switching it;
 *   - the View popup's rows are the dock list, because the reader expanded
 *     (dock-toggles) — checked here against a renderer rather than in
 *     isolation — and a toggle takes its node out of the dockspace;
 *   - an action the chrome has not wired lands in the status bar instead of
 *     doing nothing, which is the spec's own "coming soon" contract;
 *   - the spec's typography survives a 0x20..0x7E glyph atlas.
 *
 * Pixel layout is left to browser verification, exactly as the other kgui tests
 * leave it: the clicks below are computed from the reserve order rather than
 * from a screenshot, so a band changing height moves them with it.
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

static int rec_count(const char *needle)
{
	int i, n = 0;

	for (i = 0; i < g_rec_n; i++)
		if (strstr(g_rec[i], needle))
			n++;
	return n;
}

/* ------------------------------------------------------------------ */
/* The viewport, and the geometry the clicks are computed from         */
/* ------------------------------------------------------------------ */

/*
 * A desktop window, the case this chrome is shaped for. Unlike the touch-first
 * chrome it starts from the raw viewport rather than a safe frame, so the menu
 * bar sits at y=0 and the bands stack straight down from there.
 */
#define VW 1280.0f
#define VH 800.0f

#define GAP        10.0f  /* kruddgui-gap             */
#define MARGIN     16.0f  /* kruddgui-margin          */
#define SWITCH_H   44.0f  /* kruddgui-modeswitch-h    */
#define MENUBAR_H  22.0f  /* kruddgui-ig-menubar-h    */
#define TOOLBAR_H  24.0f  /* kruddgui-ig-toolbar-h    */
#define STATUS_H   20.0f  /* kruddgui-ig-status-h     */
#define TITLE_H    20.0f  /* kruddgui-ig-title-h      */
#define ROW_H      20.0f  /* kruddgui-ig-row-h        */
#define IG_PAD      8.0f  /* kruddgui-ig-pad          */
#define FRAME_PAD   4.0f  /* kruddgui-ig-frame-pad    */
#define GLYPH_W     8.0f  /* the stub atlas's advance */

/*
 * The bands, in the order kruddgui-ig-tick reserves them. Each reserve costs
 * its extent plus one gap, which is what the +GAP terms are.
 */
#define MENUBAR_Y (0.0f)
#define TOOLBAR_Y (MENUBAR_Y + MENUBAR_H + GAP)
#define CONTENT_Y (TOOLBAR_Y + TOOLBAR_H + GAP)

/*
 * The content rect's height: the viewport, less the mode-switch band reserved
 * first, less the three bands above, less the status bar off the bottom.
 */
#define SWITCH_BAND (SWITCH_H + MARGIN + GAP)
#define CONTENT_H (VH - (SWITCH_BAND + GAP) - (MENUBAR_H + GAP) \
		   - (TOOLBAR_H + GAP) - (STATUS_H + GAP))

/*
 * A menu bar item is sized to its own label, the way ImGui sizes them: the text
 * plus one window padding on each side. Every menu in this spec is four
 * characters, so the items are a uniform pitch — but it is derived, not assumed.
 */
static float menu_item_w(const char *label)
{
	return (float)strlen(label) * GLYPH_W + 2 * IG_PAD;
}

/* The x of menu INDEX, given that every item before it is the same width. */
static float menu_item_x(int index, const char *label)
{
	return index * menu_item_w(label);
}

/* ------------------------------------------------------------------ */
/* Steerable input                                                     */
/* ------------------------------------------------------------------ */

static float g_tap_x, g_tap_y;
static int   g_tap_live;

static void click(float x, float y)
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
 * A monospace stand-in for the SDF atlas: GLYPH_W per byte, 12px tall. The
 * chrome measures rather than counts, so the exact advance does not matter —
 * only that a longer run measures wider, which is what the menu popup's width,
 * the tab pitch and the right-aligned packing all key off.
 */
static s7_pointer st_metrics(s7_scheme *sc, s7_pointer a)
{
	s7_pointer s = s7_car(a);
	int        n = s7_is_string(s) ? (int)strlen(s7_string(s)) : 0;

	return s7_list(sc, 2, s7_make_real(sc, GLYPH_W * n),
		       s7_make_real(sc, 12.0));
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

/* (kgui-button x y w h) -> #t if the live click falls in the rect (once). */
static s7_pointer st_button(s7_scheme *sc, s7_pointer a)
{
	double x = num(sc, s7_car(a));
	double y = num(sc, s7_list_ref(sc, a, 1));
	double w = num(sc, s7_list_ref(sc, a, 2));
	double h = num(sc, s7_list_ref(sc, a, 3));

	if (g_tap_live && g_tap_x >= x && g_tap_x <= x + w &&
	    g_tap_y >= y && g_tap_y <= y + h) {
		g_tap_live = 0; /* consume: one click, one button */
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
	 * kgui-safe-insets is deliberately left unregistered: this chrome does
	 * not read it (a desktop window has no notch), and leaving it out keeps
	 * the rest of the image laying out against a plain margin.
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
	s7_pointer fn = s7_name_to_value(sc, "kruddgui-ig-tick");

	g_rec_n = 0;
	assert(s7_is_procedure(fn));
	s7_call(sc, fn, s7_nil(sc));
}

static void setup(void)
{
	g_rec_n    = 0;
	g_tap_live = 0;
	ev("(set! kruddgui-ig-menu #f)");
	ev("(set! kruddgui-ig-hidden '())");
	ev("(set! kruddgui-ig-front '())");
	ev("(set! kruddgui-ig-live '())");
	ev("(set! kruddgui-ig-hint \"\")");
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

/*
 * Every menu the spec declares reaches the bar, mnemonic markers stripped —
 * the spec says "&File", the atlas has no underline, the item says "File".
 */
static void test_menubar_lists_menus(void)
{
	draw();
	assert(rec_has("panel kgui-ig-menubar"));
	assert(rec_has("text File"));
	assert(rec_has("text Edit"));
	assert(rec_has("text View"));
	assert(rec_has("text Help"));
	assert(!rec_has("text &File"));
}

/*
 * The whole point of the dockspace: every dock is placed at once, in the area
 * the spec declares for it, rather than one at a time. Three nodes — Scene
 * left, Inspector right, and Assets/Console sharing one at the bottom — and a
 * central node that stays empty so the 3D view shows through.
 */
static void test_dockspace_places_every_dock(void)
{
	draw();
	assert(rec_has("panel kgui-ig-node:dock.scene"));
	assert(rec_has("panel kgui-ig-node:dock.inspector"));
	assert(rec_has("panel kgui-ig-node:dock.assets"));
	/* Console shares the Assets node rather than opening one of its own. */
	assert(!rec_has("panel kgui-ig-node:dock.console"));
	assert(rec_count("panel kgui-ig-node:") == 3);

	/* Each node's body carries the spec's own panel heading. */
	assert(rec_has("text Scene Tree"));
	assert(rec_has("text Inspector"));
	assert(rec_has("text Asset Browser"));

	assert(truthy("(= (length (kruddgui-ig-nodes (editor-layout) 'left)) 1)"));
	assert(truthy("(= (length (kruddgui-ig-nodes (editor-layout) 'right)) 1)"));
	assert(truthy("(= (length (kruddgui-ig-nodes (editor-layout) 'bottom)) 1)"));
	/* Nothing is declared for the top, so no band is reserved there. */
	assert(truthy("(null? (kruddgui-ig-nodes (editor-layout) 'top))"));
}

/*
 * The shared node draws a tab bar, forward on the member the spec raises
 * (Assets), and a click on the other tab brings it forward without disturbing
 * anything else.
 */
static void test_tab_bar_and_selection(void)
{
	float tabs_y = CONTENT_Y + CONTENT_H - (CONTENT_H * 0.28f);

	draw();
	assert(rec_has("text Assets"));
	assert(rec_has("text Console"));
	/* Forward is the raised member, so its body is what the node shows. */
	assert(rec_has("text Asset Browser"));
	assert(!rec_has("text Scheme REPL"));

	/*
	 * The Console tab sits immediately after the Assets tab, each sized to
	 * its own label. The bottom band's top edge is one bottom-fraction up
	 * from the content rect's foot.
	 */
	click(menu_item_w("Assets") + 8.0f, tabs_y + TITLE_H / 2);
	draw();
	assert(truthy("(pair? kruddgui-ig-front)"));

	/*
	 * State changes on the tick that consumes the click; the *drawing*
	 * catches up on the next one. A node resolves which member it is showing
	 * before it draws its tab bar, so this tick still paints the old body.
	 * Immediate mode has no second pass — one more tick is what makes the
	 * change visible.
	 */
	draw();
	assert(rec_has("text Scheme REPL"));
	assert(!rec_has("text Asset Browser"));
}

/* Clicking a menu bar item opens its popup; clicking it again closes it. */
static void test_menu_opens_and_closes(void)
{
	draw();
	click(menu_item_w("File") / 2, MENUBAR_H / 2);
	draw();
	assert(truthy("(eqv? kruddgui-ig-menu 0)"));

	click(menu_item_w("File") / 2, MENUBAR_H / 2);
	draw();
	assert(truthy("(not kruddgui-ig-menu)"));
}

/*
 * The File popup draws the spec's own rows, with the shortcut column filled in
 * from the standard-key symbols the spec names — the column an ImGui menu row
 * reserves on its right.
 */
static void test_popup_draws_rows(void)
{
	ev("(set! kruddgui-ig-menu 0)");
	draw();
	assert(rec_has("panel kgui-ig-popup"));
	assert(rec_has("text New Project"));
	assert(rec_has("text Open Project..."));
	assert(rec_has("text Quit"));
	assert(rec_has("text Ctrl+N"));
	assert(rec_has("text Ctrl+Shift+S"));
}

/*
 * The View popup is the dock list, because the reader expanded (dock-toggles).
 * Nothing in this file names a dock — if the spec gains one, this row appears
 * with no edit here, which is the claim being tested.
 */
static void test_view_popup_is_the_dock_list(void)
{
	ev("(set! kruddgui-ig-menu 2)");
	draw();
	assert(rec_has("text Scene"));
	assert(rec_has("text Inspector"));
	assert(rec_has("text Assets"));
	assert(rec_has("text Console"));
	assert(rec_has("text Reset Layout"));
	/* Every dock starts in the dockspace, so every toggle is checked. */
	assert(rec_count("text x") == 4);
}

/*
 * Clicking a View toggle takes that dock's node out of the dockspace. The rest
 * reflow into the space it leaves, which is what an ImGui host does when a
 * node's last window closes.
 */
static void test_toggle_removes_a_node(void)
{
	ev("(set! kruddgui-ig-menu 2)");
	draw();
	/* Row 0 of the View popup is the first dock's toggle. The popup drops
	 * under the View item and is at least 140 wide, so its middle is a
	 * safe x. */
	click(menu_item_x(2, "View") + 60.0f, CONTENT_Y + FRAME_PAD + ROW_H / 2);
	draw();
	assert(truthy("(kruddgui-ig-hidden? \"dock.scene\")"));

	/*
	 * Same frame lag as the tab bar: the dockspace paints before the popup
	 * that sits over it, so the node the click removes was already drawn by
	 * the time the row fired. One more tick is the frame that omits it.
	 */
	draw();
	assert(!rec_has("panel kgui-ig-node:dock.scene"));
	assert(rec_has("panel kgui-ig-node:dock.inspector"));
	assert(rec_count("panel kgui-ig-node:") == 2);
	assert(truthy("(null? (kruddgui-ig-nodes (editor-layout) 'left))"));
}

/* Reset Layout puts every node back and returns each tab group to the member
 * the spec raises. */
static void test_reset_layout(void)
{
	ev("(kruddgui-ig-toggle-dock! \"dock.scene\")");
	ev("(kruddgui-ig-select! (car (kruddgui-ig-nodes (editor-layout) 'bottom))"
	   " (editor-layout-dock-find (editor-layout) \"dock.console\"))");
	assert(truthy("(pair? kruddgui-ig-hidden)"));
	assert(truthy("(pair? kruddgui-ig-front)"));

	ev("(kruddgui-ig-act! \"reset-layout\")");
	assert(truthy("(null? kruddgui-ig-hidden)"));
	assert(truthy("(null? kruddgui-ig-front)"));

	draw();
	assert(rec_has("panel kgui-ig-node:dock.scene"));
	assert(rec_has("text Asset Browser"));
}

/*
 * A (tabbed-with …) naming a dock that is hidden must not take its own dock
 * down with it: Console gets a node of its own rather than becoming
 * unreachable.
 */
static void test_hidden_tab_leader_keeps_its_partner(void)
{
	ev("(kruddgui-ig-toggle-dock! \"dock.assets\")");
	draw();
	assert(rec_has("panel kgui-ig-node:dock.console"));
	assert(rec_has("text Scheme REPL"));
	assert(truthy("(= (length (kruddgui-ig-nodes (editor-layout) 'bottom)) 1)"));
}

/*
 * An action the chrome has not wired says so in the status bar. The spec is
 * explicit that which ids a host has wired is the host's business, so an
 * unknown id is a hint — never an error, and never a silent no-op.
 */
static void test_unwired_action_hints(void)
{
	ev("(kruddgui-ig-act! \"save-as\")");
	assert(truthy("(> (string-length kruddgui-ig-hint) 0)"));
	draw();
	assert(rec_has("save-as"));

	ev("(kruddgui-ig-act! \"reset-layout\")");
	assert(truthy("(= (string-length kruddgui-ig-hint) 0)"));
}

/*
 * The toolbar: the badge's seed text until the host writes over it by id, and
 * the version stamp packed against the trailing edge because the spec put a
 * (spacer) in front of it.
 */
static void test_badges(void)
{
	draw();
	assert(rec_has("panel kgui-ig-toolbar"));
	assert(rec_has("text renderer - booting..."));
	assert(rec_has("text KRUDD Editor"));

	ev("(kruddgui-ig-set! \"renderer\" \"WebGL2\")");
	ev("(kruddgui-ig-set! \"version\" \"KRUDD 19.2.1\")");
	draw();
	assert(rec_has("text WebGL2"));
	assert(rec_has("text KRUDD 19.2.1"));
	assert(!rec_has("text renderer - booting..."));
}

/*
 * The status bar carries the spec's fields by id, live text included. The
 * resolution field's seed is two em dashes around a multiplication sign, which
 * is the case the fold exists for.
 */
static void test_status_fields(void)
{
	draw();
	assert(rec_has("panel kgui-ig-status"));
	assert(rec_has("text fps -"));
	assert(rec_has("text -x-"));

	ev("(kruddgui-ig-set! \"fps\" \"60 fps\")");
	ev("(kruddgui-ig-set! \"driver\" \"WebGL2\")");
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
	assert(truthy("(string=? (kruddgui-ig-ascii \"a \xE2\x80\x94 b\")"
		      " \"a - b\")"));
	assert(truthy("(string=? (kruddgui-ig-ascii \"more\xE2\x80\xA6\")"
		      " \"more...\")"));
	assert(truthy("(string=? (kruddgui-ig-ascii \"800\xC3\x97""600\")"
		      " \"800x600\")"));
	/* Anything else non-ASCII is dropped whole, by its lead byte's
	 * continuation count, so the scan never desynchronizes. */
	assert(truthy("(string=? (kruddgui-ig-ascii \"a\xE2\x82\xAC""b\")"
		      " \"ab\")"));
	assert(truthy("(string=? (kruddgui-ig-ascii \"\") \"\")"));
	assert(truthy("(string=? (kruddgui-ig-label \"&Save \xE2\x80\xA6\")"
		      " \"Save ...\")"));
}

/*
 * A click that lands on the popup's rows is claimed by the row; one that lands
 * outside the popup dismisses it. Declaring the dismiss catcher last is what
 * makes the first half true, so this is the test that would fail if the three
 * kgui-button calls were ever reordered.
 */
static void test_click_outside_dismisses_the_popup(void)
{
	ev("(set! kruddgui-ig-menu 0)");
	draw();
	/*
	 * Far right of the content rect. The File popup opens at x=0 and is at
	 * most a couple of hundred wide, so this is outside its body — outside
	 * a row *and* outside the body's own swallow.
	 */
	click(VW - 2.0f, CONTENT_Y + 40.0f);
	draw();
	assert(truthy("(not kruddgui-ig-menu)"));
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
	assert(!rec_has("panel kgui-ig-menubar"));
	ev("(set! editor-layout editor-layout-saved)");
	draw();
	assert(rec_has("panel kgui-ig-menubar"));
}

int main(void)
{
	setup_interp();

	RUN(menubar_lists_menus);
	RUN(dockspace_places_every_dock);
	RUN(tab_bar_and_selection);
	RUN(menu_opens_and_closes);
	RUN(popup_draws_rows);
	RUN(view_popup_is_the_dock_list);
	RUN(toggle_removes_a_node);
	RUN(reset_layout);
	RUN(hidden_tab_leader_keeps_its_partner);
	RUN(unwired_action_hints);
	RUN(badges);
	RUN(status_fields);
	RUN(ascii_fold);
	RUN(click_outside_dismisses_the_popup);
	RUN(no_spec_is_a_no_op);

	printf("\n%d/%d kgui ImGui-chrome tests passed\n", tests_passed,
	       tests_run);
	return tests_passed == tests_run ? 0 : 1;
}
