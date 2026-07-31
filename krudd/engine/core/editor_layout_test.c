/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * editor_layout_test — the editor chrome spec and its reader, browser-free.
 *
 * core/editor_layout.scm is two things: (editor-layout), the tree that says
 * what the editor's chrome contains, and the editor-layout-* accessors that
 * read it. script_init evaluates the image into the shared interpreter, so a
 * test needs nothing but script_s7() to reach both.
 *
 * The reader is the half the chrome renderers share — ui/kruddgui draws this
 * tree, and whichever renderer is drawing it walks it through these accessors
 * and nothing else. That is what makes one test here worth more than a test per
 * renderer: a renderer test can only prove that *its* walk agrees with itself,
 * while this proves the walk they both start from agrees with the spec.
 *
 * Two halves:
 *
 *   1. the accessors, against the real embedded spec — every section, the
 *      dock-toggles expansion, the tab group, the mnemonic stripping;
 *   2. the degenerate inputs a spec is allowed to have — a missing section, a
 *      dock with no extras — which must read as empty rather than trap, since
 *      a renderer guarding every section before it walked it is exactly what
 *      the reader exists to avoid.
 *
 * No browser, no GPU, no canvas — just the shared s7 image, so a spec edit that
 * breaks the reader fails here long before it reaches a page.
 */
#include "script.h"

#include "s7.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int tests_run;
static int tests_passed;

#define RUN(name) do {                 \
	tests_run++;                   \
	test_##name();                 \
	tests_passed++;                \
	printf("PASS: " #name "\n");   \
} while (0)

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/*
 * Evaluate EXPR in the shared image. Every call below is a one-liner over
 * (editor-layout), so building the argument list in C would be pure noise —
 * the expression *is* the fixture, and reading it beside its expectation is
 * the point.
 */
static s7_pointer ev(const char *expr)
{
	return s7_eval_c_string(script_s7(), expr);
}

/* EXPR must evaluate to the string WANT. */
static void want_string(const char *expr, const char *want)
{
	s7_pointer v = ev(expr);

	if (!s7_is_string(v)) {
		fprintf(stderr, "%s: not a string\n", expr);
		assert(0 && "expression yields a string");
	}
	if (strcmp(s7_string(v), want)) {
		fprintf(stderr, "%s\n  want: %s\n  got:  %s\n", expr, want,
			s7_string(v));
		assert(0 && "string matches");
	}
}

/* EXPR must evaluate to the integer WANT. */
static void want_int(const char *expr, long long want)
{
	s7_pointer v = ev(expr);

	assert(s7_is_integer(v) && "expression yields an integer");
	if ((long long)s7_integer(v) != want) {
		fprintf(stderr, "%s\n  want: %lld\n  got:  %lld\n", expr,
			want, (long long)s7_integer(v));
		assert(0 && "integer matches");
	}
}

/* EXPR must evaluate to #t / #f. */
static void want_true(const char *expr)
{
	if (ev(expr) != s7_t(script_s7())) {
		fprintf(stderr, "%s: expected #t\n", expr);
		assert(0 && "expression is #t");
	}
}

static void want_false(const char *expr)
{
	if (ev(expr) != s7_f(script_s7())) {
		fprintf(stderr, "%s: expected #f\n", expr);
		assert(0 && "expression is #f");
	}
}

/*
 * EXPR must evaluate to the symbol WANT. Asked as (eq? EXPR 'WANT) inside the
 * image rather than by reading the symbol's name out through the C API: symbol
 * identity is exactly what eq? answers, and it keeps this test on the same
 * small slice of s7.h the rest of the tree already uses.
 */
static void want_symbol(const char *expr, const char *want)
{
	char buf[1024];

	snprintf(buf, sizeof(buf), "(eq? %s '%s)", expr, want);
	want_true(buf);
}

/* ------------------------------------------------------------------ */
/* Half 1 — the accessors over the real spec                           */
/* ------------------------------------------------------------------ */

/*
 * The spec evaluates to a tree with the four sections the doc describes, and
 * each reads back as a non-empty list. This is the assertion every other test
 * here stands on, so it is made once, plainly, rather than assumed.
 */
static void test_sections_present(void)
{
	want_true("(pair? (editor-layout))");
	want_int("(length (editor-layout-menus (editor-layout)))", 4);
	want_int("(length (editor-layout-toolbar (editor-layout)))", 3);
	want_int("(length (editor-layout-docks (editor-layout)))", 4);
	want_int("(length (editor-layout-statusbar (editor-layout)))", 3);
}

/* The menu bar, in declaration order, with its mnemonics intact in the spec. */
static void test_menus(void)
{
	want_string("(editor-layout-menu-label"
		    " (car (editor-layout-menus (editor-layout))))",
		    "&File");
	want_string("(editor-layout-menu-label"
		    " (list-ref (editor-layout-menus (editor-layout)) 3))",
		    "&Help");

	/* File's first item: an action with a standard-key shortcut symbol. */
	want_symbol("(editor-layout-kind"
		    " (car (editor-layout-menu-items"
		    "       (car (editor-layout-menus (editor-layout)))"
		    "       (editor-layout-docks (editor-layout)))))",
		    "action");
	want_string("(editor-layout-action-label"
		    " (car (editor-layout-menu-items"
		    "       (car (editor-layout-menus (editor-layout)))"
		    "       (editor-layout-docks (editor-layout)))))",
		    "&New Project");
	want_symbol("(editor-layout-action-shortcut"
		    " (car (editor-layout-menu-items"
		    "       (car (editor-layout-menus (editor-layout)))"
		    "       (editor-layout-docks (editor-layout)))))",
		    "new");
	want_string("(editor-layout-action-id"
		    " (car (editor-layout-menu-items"
		    "       (car (editor-layout-menus (editor-layout)))"
		    "       (editor-layout-docks (editor-layout)))))",
		    "new");

	/* Separators survive the walk as their own bare form. */
	want_symbol("(editor-layout-kind"
		    " (list-ref (editor-layout-menu-items"
		    "            (car (editor-layout-menus (editor-layout)))"
		    "            (editor-layout-docks (editor-layout))) 2))",
		    "separator");
}

/*
 * The View menu is the one place the menus section and the docks section have
 * to agree. (dock-toggles) is one form in the spec and must come back out of
 * the reader as one toggle per dock, in declaration order, in the position the
 * marker held — so the separator and Reset Layout that follow it stay after
 * the toggles rather than being pushed to the front.
 */
static void test_dock_toggles_expand(void)
{
	const char *view =
		"(editor-layout-menu-items"
		" (list-ref (editor-layout-menus (editor-layout)) 2)"
		" (editor-layout-docks (editor-layout)))";
	char expr[512];

	/* Four docks + separator + Reset Layout. */
	snprintf(expr, sizeof(expr), "(length %s)", view);
	want_int(expr, 6);

	/* The marker itself is gone. */
	snprintf(expr, sizeof(expr),
		 "(let loop ((i %s)) (cond ((null? i) #f)"
		 " ((eq? (editor-layout-kind (car i)) 'dock-toggles) #t)"
		 " (else (loop (cdr i)))))", view);
	want_false(expr);

	/* Toggle 0 is the first dock, labelled by its title, wired by id. */
	snprintf(expr, sizeof(expr),
		 "(editor-layout-action-label (car %s))", view);
	want_string(expr, "Scene");
	snprintf(expr, sizeof(expr),
		 "(editor-layout-action-id (car %s))", view);
	want_string(expr, "toggle:dock.scene");

	/* Toggle 3 is the last dock — declaration order is preserved. */
	snprintf(expr, sizeof(expr),
		 "(editor-layout-action-id (list-ref %s 3))", view);
	want_string(expr, "toggle:dock.console");

	/* What followed the marker still follows the expansion. */
	snprintf(expr, sizeof(expr),
		 "(editor-layout-kind (list-ref %s 4))", view);
	want_symbol(expr, "separator");
	snprintf(expr, sizeof(expr),
		 "(editor-layout-action-id (list-ref %s 5))", view);
	want_string(expr, "reset-layout");
}

/*
 * The toolbar's badge/spacer vocabulary. The renderer badge is seeded with no
 * backend name on purpose (the spec cannot know which device wins the async
 * handshake), and the spacer is what puts the version stamp against the
 * trailing edge — both are contracts a renderer reads, so both are pinned.
 */
static void test_toolbar(void)
{
	want_symbol("(editor-layout-kind"
		    " (car (editor-layout-toolbar (editor-layout))))",
		    "badge");
	want_string("(editor-layout-badge-id"
		    " (car (editor-layout-toolbar (editor-layout))))",
		    "renderer");
	want_string("(editor-layout-badge-text"
		    " (car (editor-layout-toolbar (editor-layout))))",
		    "renderer \xE2\x80\x94 booting\xE2\x80\xA6");

	want_symbol("(editor-layout-kind"
		    " (list-ref (editor-layout-toolbar (editor-layout)) 1))",
		    "spacer");
	want_string("(editor-layout-badge-id"
		    " (list-ref (editor-layout-toolbar (editor-layout)) 2))",
		    "version");
}

/* The docks: identity, title, area, the placeholder copy, and the tab group. */
static void test_docks(void)
{
	const char *scene = "(editor-layout-dock-find (editor-layout) \"dock.scene\")";
	const char *console =
		"(editor-layout-dock-find (editor-layout) \"dock.console\")";
	const char *assets =
		"(editor-layout-dock-find (editor-layout) \"dock.assets\")";
	char expr[512];

	snprintf(expr, sizeof(expr), "(editor-layout-dock-title %s)", scene);
	want_string(expr, "Scene");
	snprintf(expr, sizeof(expr), "(editor-layout-dock-area %s)", scene);
	want_symbol(expr, "left");
	snprintf(expr, sizeof(expr), "(editor-layout-dock-panel %s)", scene);
	want_string(expr, "Scene Tree");
	snprintf(expr, sizeof(expr),
		 "(> (string-length (editor-layout-dock-blurb %s)) 0)", scene);
	want_true(expr);

	/* Console tabs behind Assets; Assets is the raised one. */
	snprintf(expr, sizeof(expr),
		 "(editor-layout-dock-tabbed-with %s)", console);
	want_string(expr, "dock.assets");
	snprintf(expr, sizeof(expr), "(editor-layout-dock-raise? %s)", assets);
	want_true(expr);

	/* A dock with no extras answers both questions without trapping. */
	snprintf(expr, sizeof(expr),
		 "(editor-layout-dock-tabbed-with %s)", scene);
	want_false(expr);
	snprintf(expr, sizeof(expr), "(editor-layout-dock-raise? %s)", scene);
	want_false(expr);

	/* An id no dock carries is #f, not a trap. */
	want_false("(editor-layout-dock-find (editor-layout) \"dock.nope\")");
}

/*
 * Placement by area — the walk both renderers start from. Left and right hold
 * one dock each; bottom holds the two that share a tab group; nothing is
 * declared for top, which must read as the empty list.
 */
static void test_docks_by_area(void)
{
	want_int("(length (editor-layout-docks-in (editor-layout) 'left))", 1);
	want_int("(length (editor-layout-docks-in (editor-layout) 'right))", 1);
	want_int("(length (editor-layout-docks-in (editor-layout) 'bottom))", 2);
	want_int("(length (editor-layout-docks-in (editor-layout) 'top))", 0);

	/* Order within an area is declaration order: Assets before Console. */
	want_string("(editor-layout-dock-id"
		    " (car (editor-layout-docks-in (editor-layout) 'bottom)))",
		    "dock.assets");
}

/* The status bar, including the field the spec seeds empty on purpose. */
static void test_statusbar(void)
{
	want_string("(editor-layout-field-id"
		    " (car (editor-layout-statusbar (editor-layout))))",
		    "fps");
	want_string("(editor-layout-field-text"
		    " (car (editor-layout-statusbar (editor-layout))))",
		    "fps \xE2\x80\x94");
	want_string("(editor-layout-field-id"
		    " (list-ref (editor-layout-statusbar (editor-layout)) 2))",
		    "driver");
	want_string("(editor-layout-field-text"
		    " (list-ref (editor-layout-statusbar (editor-layout)) 2))",
		    "");
}

/*
 * Mnemonic stripping. The spec marks a menu's shortcut letter with `&`, which
 * is a DOM menu's convention; a renderer drawing its own glyphs has nowhere to
 * put the underline and wants the plain string. The marker can sit anywhere in
 * the label, and a doubled `&&` is a literal ampersand rather than a marker.
 */
static void test_mnemonic(void)
{
	want_string("(editor-layout-mnemonic \"&New Project\")", "New Project");
	want_string("(editor-layout-mnemonic \"Cu&t\")", "Cut");
	want_string("(editor-layout-mnemonic \"Save &As\xE2\x80\xA6\")",
		    "Save As\xE2\x80\xA6");
	want_string("(editor-layout-mnemonic \"plain\")", "plain");
	want_string("(editor-layout-mnemonic \"\")", "");
	want_string("(editor-layout-mnemonic \"Fish && Chips\")",
		    "Fish & Chips");
	/* A trailing marker with nothing to mark drops, rather than trapping on
	 * the lookahead past the end of the string. */
	want_string("(editor-layout-mnemonic \"trail&\")", "trail");
}

/* ------------------------------------------------------------------ */
/* Half 2 — degenerate specs read as empty, not as errors              */
/* ------------------------------------------------------------------ */

/*
 * A spec is allowed to declare fewer sections than the full one does — a
 * chrome with no toolbar is a coherent thing to ask for. The reader answers
 * with the empty list so a renderer's walk simply draws nothing, which is why
 * no renderer guards a section before walking it.
 */
static void test_missing_sections(void)
{
	want_int("(length (editor-layout-menus '((docks))))", 0);
	want_int("(length (editor-layout-toolbar '((menus))))", 0);
	want_int("(length (editor-layout-docks '()))", 0);
	want_int("(length (editor-layout-statusbar '()))", 0);
	want_int("(length (editor-layout-docks-in '() 'left))", 0);
	want_false("(editor-layout-dock-find '() \"dock.scene\")");

	/* A View menu expanded against no docks is the marker removed and
	 * nothing put in its place — not a trap, and not the marker left in. */
	want_int("(length (editor-layout-menu-items"
		 " '(menu \"&View\" (dock-toggles) (separator)) '()))", 1);
	want_symbol("(editor-layout-kind"
		    " (car (editor-layout-menu-items"
		    "       '(menu \"&View\" (dock-toggles) (separator)) '())))",
		    "separator");

	/* A non-pair where a node was expected has no kind, rather than
	 * taking the car of a non-pair. */
	want_false("(editor-layout-kind '())");
	want_false("(editor-layout-kind 7)");
}

int main(void)
{
	s7_scheme *sc = script_s7(); /* starts the interpreter on demand */

	assert(sc && "the s7 interpreter must start");
	assert(s7_is_procedure(s7_name_to_value(sc, "editor-layout")) &&
	       "script_init evaluates the chrome spec into the shared image");

	RUN(sections_present);
	RUN(menus);
	RUN(dock_toggles_expand);
	RUN(toolbar);
	RUN(docks);
	RUN(docks_by_area);
	RUN(statusbar);
	RUN(mnemonic);
	RUN(missing_sections);

	printf("\n%d/%d editor layout tests passed\n", tests_passed, tests_run);
	return tests_passed == tests_run ? 0 : 1;
}
