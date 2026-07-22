/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * script_layout_json_test — the s7->JS serialization primitive, browser-free.
 *
 * The web editor (#706 part C) cannot hold an s7 value, so the layout crosses
 * the seam as a JSON string script_layout_json() produces; part C's DOM builder
 * JSON.parses it. This test closes the loop the browser can't close in CI: it
 * runs the same serializer natively and asserts on the string, in two halves —
 *
 *   1. script_json() on hand-built s7 values, so the value->JSON contract
 *      (arrays, strings, symbols, numbers, booleans, (), and JSON escaping) is
 *      pinned against inputs the fixed layout never exercises (quotes,
 *      backslashes, control chars, nesting).
 *   2. script_layout_json() on the real embedded spec, asserting the evaluated
 *      (editor-layout) tree round-trips to the JSON shape the web chrome reads.
 *
 * No browser, no GPU, no Qt — just the shared s7 image, so a broken walk or a
 * malformed escape fails here long before it reaches a page.
 */
#include "script.h"

#include "s7.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* script_json of the single value VALUE, asserted equal to WANT. */
static void check_json(s7_scheme *sc, s7_pointer value, const char *want)
{
	const char *got = script_json(value);

	assert(got && "script_json returns a string for a live value");
	if (strcmp(got, want)) {
		fprintf(stderr, "script_json mismatch\n  want: %s\n  got: %s\n",
			want, got);
		assert(0 && "script_json output matches");
	}
	(void)sc;
}

/* ---- half 1: the generic value -> JSON contract ------------------------- */
static void test_value_json(s7_scheme *sc)
{
	/* Scalars and the empty list, each on its own. */
	check_json(sc, s7_make_integer(sc, 42), "42");
	check_json(sc, s7_make_integer(sc, -7), "-7");
	check_json(sc, s7_make_string(sc, "hi"), "\"hi\"");
	check_json(sc, s7_make_symbol(sc, "left"), "\"left\"");
	check_json(sc, s7_make_boolean(sc, true), "true");
	check_json(sc, s7_make_boolean(sc, false), "false");
	check_json(sc, s7_nil(sc), "[]");

	/* A real serializes as a JSON number (the layout has none, but the
	 * primitive is generic). */
	check_json(sc, s7_make_real(sc, 1.5), "1.5");

	/* JSON string escaping: a quote and a backslash get the two-char short
	 * escapes; the surrounding bytes pass through. */
	check_json(sc, s7_make_string(sc, "a\"b\\c"), "\"a\\\"b\\\\c\"");

	/* The named control escapes. */
	check_json(sc, s7_make_string(sc, "x\n\ty"), "\"x\\n\\ty\"");

	/* A C0 control with no short escape becomes \u00XX (lower-case hex). */
	check_json(sc, s7_make_string(sc, "\x01"), "\"\\u0001\"");

	/* Multi-byte UTF-8 is valid inside a JSON string and passes verbatim —
	 * the em dash and ellipsis the layout is full of. */
	check_json(sc, s7_make_string(sc, "—…"), "\"—…\"");

	/* A list becomes an array; a symbol head reads back as a string, so a
	 * tagged form (action ...) serializes shape-for-shape. */
	check_json(sc,
		   s7_list(sc, 3, s7_make_symbol(sc, "action"),
			   s7_make_string(sc, "&Quit"),
			   s7_make_string(sc, "quit")),
		   "[\"action\",\"&Quit\",\"quit\"]");

	/* Nesting and the empty-list-as-empty-array case together. */
	check_json(sc,
		   s7_list(sc, 2,
			   s7_list(sc, 2, s7_make_integer(sc, 1),
				   s7_make_integer(sc, 2)),
			   s7_nil(sc)),
		   "[[1,2],[]]");

	printf("script_json: value->JSON contract holds\n");
}

/* got must contain needle. */
static void must_contain(const char *got, const char *needle)
{
	if (!strstr(got, needle)) {
		fprintf(stderr, "layout JSON missing: %s\n", needle);
		assert(0 && "layout JSON contains the expected fragment");
	}
}

/* ---- half 2: the real editor layout spec -> JSON ------------------------ */
static void test_layout_json(void)
{
	const char *j = script_layout_json();
	size_t      n;

	assert(j && "the embedded layout spec serializes to JSON");
	n = strlen(j);
	/* The whole tree is a JSON array of sections. */
	assert(n >= 2 && j[0] == '[' && j[n - 1] == ']' &&
	       "layout JSON is a JSON array");

	/* Sections, each a tagged array headed by its section name. */
	must_contain(j, "[\"menus\",");
	must_contain(j, "[\"toolbar\",");
	must_contain(j, "[\"docks\",");
	must_contain(j, "[\"statusbar\",");

	/* A wired File action: label, shortcut symbol (as a string), action id.
	 * The ellipsis proves multi-byte UTF-8 survives the seam. */
	must_contain(j,
		"[\"action\",\"&Open Project…\",\"open\",\"open-project\"]");
	/* The View menu's dock-toggles expansion marker, a nullary form. */
	must_contain(j, "[\"dock-toggles\"]");

	/* The toolbar's live badge — em dash and ellipsis pass through. */
	must_contain(j,
		     "[\"badge\",\"renderer\",\"Vulkan — booting…\"]");

	/* The Console dock: its area symbol as a string, and its tab group. */
	must_contain(j, "[\"dock\",\"dock.console\",\"Console\",\"bottom\",");
	must_contain(j, "[\"tabbed-with\",\"dock.assets\"]");
	/* Assets is raised above the group — a nullary marker form. */
	must_contain(j, "[\"raise\"]");

	/* Status fields, including the empty-string driver field. */
	must_contain(j, "[\"field\",\"fps\",\"fps \xE2\x80\x94\"]");
	must_contain(j, "[\"field\",\"driver\",\"\"]");

	printf("script_layout_json: %zu bytes, shape holds\n", n);
}

int main(void)
{
	s7_scheme *sc = script_s7(); /* starts the interpreter on demand */

	assert(sc && "the s7 interpreter must start");

	test_value_json(sc);
	test_layout_json();

	printf("script_layout_json tests passed\n");
	return 0;
}
