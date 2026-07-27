/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Host tests for the GL-free editable-text core (kgui_text_edit) — the string
 * surgery behind kruddgui's soft-keyboard field. They drive insert / backspace
 * / delete and the caret moves the way the char/key drain in kruddgui.cpp will,
 * asserting on the buffer and caret, with no GL and no keyboard bridge.
 */

#include "kgui_text_edit.h"

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

/* Insert a whole C string at the caret (the common "type a run" path). */
static int type(struct kgui_text_edit *e, const char *s)
{
	return kgui_text_edit_insert(e, s, (int)strlen(s));
}

/*
 * A deliberately *proportional* width stub for the multiline caret tests: 'i'
 * and '.' are narrow (1), 'W' and 'm' wide (3), every other ASCII glyph 2, and
 * any multi-byte code point 2. This makes a character column and a pixel column
 * disagree, so a test can prove up/down snap the caret to the nearest pixel
 * boundary rather than to a fixed character count.
 */
static float measure_stub(const char *s, int n, void *ud)
{
	float w = 0.0f;
	int   i = 0;

	(void)ud;
	while (i < n) {
		unsigned char c  = (unsigned char)s[i];
		int           cl = 1;

		if (c >= 0x80) {
			if ((c & 0xE0) == 0xC0)
				cl = 2;
			else if ((c & 0xF0) == 0xE0)
				cl = 3;
			else if ((c & 0xF8) == 0xF0)
				cl = 4;
			w += 2.0f;
		} else if (c == 'i' || c == '.') {
			w += 1.0f;
		} else if (c == 'W' || c == 'm') {
			w += 3.0f;
		} else {
			w += 2.0f;
		}
		i += cl;
		if (i > n)
			i = n;
	}
	return w;
}

static void test_set_seeds_caret_at_end(void)
{
	struct kgui_text_edit e;

	kgui_text_edit_set(&e, "hello");
	assert(strcmp(e.buf, "hello") == 0);
	assert(e.len == 5);
	assert(e.caret == 5);
}

static void test_insert_appends_at_caret(void)
{
	struct kgui_text_edit e;

	kgui_text_edit_set(&e, "ab");
	assert(type(&e, "c") == 1);
	assert(strcmp(e.buf, "abc") == 0);
	assert(e.caret == 3);
}

static void test_insert_mid_string(void)
{
	struct kgui_text_edit e;

	kgui_text_edit_set(&e, "ac");
	kgui_text_edit_left(&e);        /* caret between a and c */
	assert(e.caret == 1);
	type(&e, "b");
	assert(strcmp(e.buf, "abc") == 0);
	assert(e.caret == 2);
}

static void test_backspace_deletes_before_caret(void)
{
	struct kgui_text_edit e;

	kgui_text_edit_set(&e, "abc");
	kgui_text_edit_backspace(&e);
	assert(strcmp(e.buf, "ab") == 0);
	assert(e.caret == 2);
	kgui_text_edit_home(&e);
	kgui_text_edit_backspace(&e);   /* no-op at start */
	assert(strcmp(e.buf, "ab") == 0);
	assert(e.caret == 0);
}

static void test_delete_fwd_at_caret(void)
{
	struct kgui_text_edit e;

	kgui_text_edit_set(&e, "abc");
	kgui_text_edit_home(&e);
	kgui_text_edit_delete_fwd(&e);
	assert(strcmp(e.buf, "bc") == 0);
	assert(e.caret == 0);
	kgui_text_edit_end(&e);
	kgui_text_edit_delete_fwd(&e);  /* no-op at end */
	assert(strcmp(e.buf, "bc") == 0);
}

static void test_caret_moves_and_clamps(void)
{
	struct kgui_text_edit e;

	kgui_text_edit_set(&e, "xy");
	kgui_text_edit_end(&e);
	kgui_text_edit_right(&e);       /* clamped */
	assert(e.caret == 2);
	kgui_text_edit_left(&e);
	kgui_text_edit_left(&e);
	kgui_text_edit_left(&e);        /* clamped */
	assert(e.caret == 0);
}

/* A 2-byte code point (é = C3 A9) is inserted and stepped over whole. */
static void test_utf8_boundaries(void)
{
	struct kgui_text_edit e;

	kgui_text_edit_clear(&e);
	assert(type(&e, "a\xC3\xA9""b") == 4);   /* a, é, b */
	assert(e.len == 4);
	assert(e.caret == 4);

	kgui_text_edit_home(&e);
	kgui_text_edit_right(&e);       /* past 'a' */
	assert(e.caret == 1);
	kgui_text_edit_right(&e);       /* past the whole 2-byte 'é' */
	assert(e.caret == 3);

	kgui_text_edit_backspace(&e);   /* removes the whole 'é' */
	assert(strcmp(e.buf, "ab") == 0);
	assert(e.caret == 1);
}

static void test_insert_respects_capacity(void)
{
	struct kgui_text_edit e;
	char                  big[KGUI_TEXT_EDIT_CAP + 32];
	int                   i;

	kgui_text_edit_clear(&e);
	for (i = 0; i < (int)sizeof(big) - 1; i++)
		big[i] = 'z';
	big[sizeof(big) - 1] = '\0';

	type(&e, big);
	assert(e.len <= KGUI_TEXT_EDIT_CAP - 1);
	assert(e.buf[e.len] == '\0');
	/* One more never overflows. */
	type(&e, "!");
	assert(e.len <= KGUI_TEXT_EDIT_CAP - 1);
}

/* A newline is an ordinary inserted byte; the line count follows it. */
static void test_newline_inserts_and_counts(void)
{
	struct kgui_text_edit e;
	int                   line, col;

	kgui_text_edit_set(&e, "ab");
	assert(kgui_text_edit_line_count(&e) == 1);
	assert(type(&e, "\n") == 1);
	assert(strcmp(e.buf, "ab\n") == 0);
	assert(e.caret == 3);
	assert(kgui_text_edit_line_count(&e) == 2);

	type(&e, "cd");
	assert(strcmp(e.buf, "ab\ncd") == 0);
	kgui_text_edit_caret(&e, measure_stub, NULL, &line, &col, NULL);
	assert(line == 1);
	assert(col == 2);
}

/* Home/End are line-scoped for multiline, unlike the buffer-wide home/end. */
static void test_line_home_end(void)
{
	struct kgui_text_edit e;

	kgui_text_edit_set(&e, "ab\ncd");    /* caret at end (5), on line 1 */
	kgui_text_edit_line_home(&e);
	assert(e.caret == 3);                 /* start of line 1, not buffer */
	kgui_text_edit_line_end(&e);
	assert(e.caret == 5);

	kgui_text_edit_home(&e);              /* buffer home -> line 0 start */
	assert(e.caret == 0);
	kgui_text_edit_line_end(&e);
	assert(e.caret == 2);                 /* stops before the '\n' */
	kgui_text_edit_line_home(&e);
	assert(e.caret == 0);
}

/* Up/Down snap to the nearest *pixel* boundary, not the same char column. */
static void test_up_down_snap_to_pixel(void)
{
	struct kgui_text_edit e;

	/*
	 * Line 0 "iimm": boundary x = 0,1,2,5,8.  Line 1 "mmii": x = 0,3,6,7,8.
	 * Caret after "iim" on line 0 is x = 5 (byte 3). Down holds x 5; line 1's
	 * nearest boundary is x 6 (byte offset 2 -> caret 7), beating x 3.
	 */
	kgui_text_edit_set(&e, "iimm\nmmii");
	kgui_text_edit_home(&e);
	kgui_text_edit_right(&e);
	kgui_text_edit_right(&e);
	kgui_text_edit_right(&e);             /* caret 3, x = 5 */
	kgui_text_edit_down(&e, measure_stub, NULL);
	assert(e.caret == 7);                 /* snapped to x 6, not char col 3 */
}

/* The target x is held across consecutive up/down moves (no drift). */
static void test_up_down_no_drift(void)
{
	struct kgui_text_edit e;

	/*
	 * Line 0/2 "mmmmmm" (x steps of 3, end x = 18), line 1 "i" (end x = 1).
	 * From the end of line 0, Down lands on line 1's only real column, then
	 * Down again must recover x 18 on line 2 — proving the goal survived the
	 * narrow middle line rather than collapsing to its x 1.
	 */
	kgui_text_edit_set(&e, "mmmmmm\ni\nmmmmmm");
	kgui_text_edit_home(&e);
	kgui_text_edit_line_end(&e);          /* caret 6, x = 18 */
	kgui_text_edit_down(&e, measure_stub, NULL);
	assert(e.caret == 8);                 /* end of the narrow line "i" */
	kgui_text_edit_down(&e, measure_stub, NULL);
	assert(e.caret == 15);                /* end of line 2, x recovered to 18 */

	/* A horizontal edit clears the goal, so the next Up recomputes it. */
	kgui_text_edit_left(&e);              /* caret 14, x = 15 */
	kgui_text_edit_up(&e, measure_stub, NULL);
	assert(e.caret == 8);                 /* line 1 only reaches x 1 */
}

/* Up on the first line and Down on the last line are no-ops. */
static void test_up_down_clamp_at_edges(void)
{
	struct kgui_text_edit e;

	kgui_text_edit_set(&e, "ab\ncd");
	kgui_text_edit_home(&e);              /* line 0, caret 0 */
	kgui_text_edit_up(&e, measure_stub, NULL);
	assert(e.caret == 0);                 /* already on the first line */

	kgui_text_edit_end(&e);               /* line 1, caret 5 */
	kgui_text_edit_down(&e, measure_stub, NULL);
	assert(e.caret == 5);                 /* already on the last line */
}

/* The caret report gives line, column (code points), and pixel x within a line. */
static void test_caret_report(void)
{
	struct kgui_text_edit e;
	int                   line, col;
	float                 px;

	/* é is two bytes; the caret sits after "a" + "é" on line 1 (byte 6). */
	kgui_text_edit_set(&e, "xy\na\xC3\xA9z");
	kgui_text_edit_line_home(&e);         /* line 1 start (byte 3) */
	kgui_text_edit_right(&e);             /* past 'a' */
	kgui_text_edit_right(&e);             /* past the whole 'é' */
	kgui_text_edit_caret(&e, measure_stub, NULL, &line, &col, &px);
	assert(line == 1);
	assert(col == 2);                     /* two code points, not three bytes */
	assert(px == 4.0f);                   /* 'a' (2) + 'é' (2) */
}

int main(void)
{
	RUN(set_seeds_caret_at_end);
	RUN(insert_appends_at_caret);
	RUN(insert_mid_string);
	RUN(backspace_deletes_before_caret);
	RUN(delete_fwd_at_caret);
	RUN(caret_moves_and_clamps);
	RUN(utf8_boundaries);
	RUN(insert_respects_capacity);
	RUN(newline_inserts_and_counts);
	RUN(line_home_end);
	RUN(up_down_snap_to_pixel);
	RUN(up_down_no_drift);
	RUN(up_down_clamp_at_edges);
	RUN(caret_report);

	printf("\n%d/%d kgui_text_edit tests passed\n", tests_passed, tests_run);
	return tests_passed == tests_run ? 0 : 1;
}
