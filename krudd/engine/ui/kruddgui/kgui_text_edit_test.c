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

	printf("\n%d/%d kgui_text_edit tests passed\n", tests_passed, tests_run);
	return tests_passed == tests_run ? 0 : 1;
}
