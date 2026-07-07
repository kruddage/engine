/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * md_parse_test.c — unit tests for the dependency-free markdown parser.
 *
 * Each test_*() function returns 0 on pass, 1 on failure.
 * main() aggregates failures and exits with the count.
 */

#include "md_parse.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Test infrastructure                                                 */
/* ------------------------------------------------------------------ */

static int g_failures;

#define CHECK(expr)							\
	do {								\
		if (!(expr)) {						\
			fprintf(stderr,					\
				"FAIL %s:%d  %s\n",			\
				__FILE__, __LINE__, #expr);		\
			g_failures++;					\
		}							\
	} while (0)

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static int find_span(const struct md_block *blk, uint32_t style)
{
	uint32_t i;

	for (i = 0; i < blk->span_count; i++) {
		if (blk->spans[i].style == style)
			return (int)i;
	}
	return -1;
}

/* ------------------------------------------------------------------ */
/* Test: empty input                                                   */
/* ------------------------------------------------------------------ */

static void test_empty(void)
{
	struct md_block out[4];
	int32_t n;

	n = md_parse("", out, 4);
	CHECK(n == 0);

	/* NULL src */
	n = md_parse(NULL, out, 4);
	CHECK(n == 0);

	/* max == 0 */
	n = md_parse("# Hello", out, 0);
	CHECK(n == 0);

	/* whitespace-only lines */
	n = md_parse("   \n\t\n  ", out, 4);
	CHECK(n == 0);
}

/* ------------------------------------------------------------------ */
/* Test: headings                                                      */
/* ------------------------------------------------------------------ */

static void test_headings(void)
{
	static const char src[] =
		"# Heading One\n"
		"## Heading Two\n"
		"### Heading Three\n";
	struct md_block out[8];
	int32_t         n;

	n = md_parse(src, out, 8);
	CHECK(n == 3);

	CHECK(out[0].type  == MD_BLOCK_HEADING);
	CHECK(out[0].level == 1);
	CHECK(strcmp(out[0].text, "Heading One") == 0);

	CHECK(out[1].type  == MD_BLOCK_HEADING);
	CHECK(out[1].level == 2);
	CHECK(strcmp(out[1].text, "Heading Two") == 0);

	CHECK(out[2].type  == MD_BLOCK_HEADING);
	CHECK(out[2].level == 3);
	CHECK(strcmp(out[2].text, "Heading Three") == 0);
}

/* ------------------------------------------------------------------ */
/* Test: heading level > 3 treated as paragraph                       */
/* ------------------------------------------------------------------ */

static void test_heading_overflow(void)
{
	static const char src[] = "#### Not a heading\n";
	struct md_block   out[4];
	int32_t           n;

	n = md_parse(src, out, 4);
	CHECK(n == 1);
	CHECK(out[0].type != MD_BLOCK_HEADING);
	CHECK(out[0].type == MD_BLOCK_PARAGRAPH);
}

/* ------------------------------------------------------------------ */
/* Test: paragraphs                                                    */
/* ------------------------------------------------------------------ */

static void test_paragraphs(void)
{
	static const char src[] =
		"First paragraph.\n"
		"\n"
		"Second paragraph.\n";
	struct md_block out[8];
	int32_t         n;

	n = md_parse(src, out, 8);
	CHECK(n == 2);
	CHECK(out[0].type == MD_BLOCK_PARAGRAPH);
	CHECK(strcmp(out[0].text, "First paragraph.") == 0);
	CHECK(out[1].type == MD_BLOCK_PARAGRAPH);
	CHECK(strcmp(out[1].text, "Second paragraph.") == 0);
}

/* ------------------------------------------------------------------ */
/* Test: unordered list items (- and *)                                */
/* ------------------------------------------------------------------ */

static void test_list_items(void)
{
	static const char src[] =
		"- item one\n"
		"* item two\n"
		"- item three\n";
	struct md_block out[8];
	int32_t         n;

	n = md_parse(src, out, 8);
	CHECK(n == 3);

	CHECK(out[0].type == MD_BLOCK_LIST_ITEM);
	CHECK(strcmp(out[0].text, "item one") == 0);

	CHECK(out[1].type == MD_BLOCK_LIST_ITEM);
	CHECK(strcmp(out[1].text, "item two") == 0);

	CHECK(out[2].type == MD_BLOCK_LIST_ITEM);
	CHECK(strcmp(out[2].text, "item three") == 0);
}

/* ------------------------------------------------------------------ */
/* Test: fenced code blocks                                            */
/* ------------------------------------------------------------------ */

static void test_code_fence(void)
{
	static const char src[] =
		"```\n"
		"int x = 42;\n"
		"return x;\n"
		"```\n"
		"After fence.\n";
	struct md_block out[16];
	int32_t         n;
	int             i;
	int             found_para = 0;

	n = md_parse(src, out, 16);

	/* Two interior lines + one paragraph = 3 blocks. */
	CHECK(n == 3);

	for (i = 0; i < n; i++) {
		if (out[i].type == MD_BLOCK_PARAGRAPH)
			found_para = 1;
	}
	CHECK(found_para);

	CHECK(out[0].type == MD_BLOCK_CODE);
	CHECK(strcmp(out[0].text, "int x = 42;") == 0);
	CHECK(out[1].type == MD_BLOCK_CODE);
	CHECK(strcmp(out[1].text, "return x;") == 0);
}

/* ------------------------------------------------------------------ */
/* Test: unterminated fence — silently closed at EOF                  */
/* ------------------------------------------------------------------ */

static void test_unterminated_fence(void)
{
	static const char src[] =
		"```\n"
		"line one\n"
		"line two\n";
	struct md_block out[8];
	int32_t         n;

	n = md_parse(src, out, 8);
	/* Two interior lines; no closing fence. */
	CHECK(n == 2);
	CHECK(out[0].type == MD_BLOCK_CODE);
	CHECK(out[1].type == MD_BLOCK_CODE);
}

/* ------------------------------------------------------------------ */
/* Test: inline bold                                                   */
/* ------------------------------------------------------------------ */

static void test_inline_bold(void)
{
	static const char src[] = "This is **bold** text.\n";
	struct md_block   out[4];
	int32_t           n;
	int               si;

	n = md_parse(src, out, 4);
	CHECK(n == 1);
	CHECK(out[0].type == MD_BLOCK_PARAGRAPH);

	/* Delimiters are stripped from the block text. */
	CHECK(strcmp(out[0].text, "This is bold text.") == 0);

	si = find_span(&out[0], MD_SPAN_BOLD);
	CHECK(si >= 0);
	if (si >= 0) {
		uint32_t s = out[0].spans[si].start;
		uint32_t e = out[0].spans[si].end;
		char     buf[32];
		uint32_t len = e - s;

		if (len >= sizeof(buf))
			len = sizeof(buf) - 1;
		memcpy(buf, out[0].text + s, len);
		buf[len] = '\0';
		CHECK(strcmp(buf, "bold") == 0);
	}
}

/* ------------------------------------------------------------------ */
/* Test: inline code                                                   */
/* ------------------------------------------------------------------ */

static void test_inline_code(void)
{
	static const char src[] = "Call `engine_tick()` each frame.\n";
	struct md_block   out[4];
	int32_t           n;
	int               si;

	n = md_parse(src, out, 4);
	CHECK(n == 1);

	/* Backticks are stripped from the block text. */
	CHECK(strcmp(out[0].text, "Call engine_tick() each frame.") == 0);

	si = find_span(&out[0], MD_SPAN_CODE);
	CHECK(si >= 0);
	if (si >= 0) {
		uint32_t s = out[0].spans[si].start;
		uint32_t e = out[0].spans[si].end;
		char     buf[64];
		uint32_t len = e - s;

		if (len >= sizeof(buf))
			len = sizeof(buf) - 1;
		memcpy(buf, out[0].text + s, len);
		buf[len] = '\0';
		CHECK(strcmp(buf, "engine_tick()") == 0);
	}
}

/* ------------------------------------------------------------------ */
/* Test: mixed inline bold and code                                    */
/* ------------------------------------------------------------------ */

static void test_inline_mixed(void)
{
	static const char src[] =
		"Use **bold** and `code` together.\n";
	struct md_block   out[4];
	int32_t           n;

	n = md_parse(src, out, 4);
	CHECK(n == 1);
	CHECK(out[0].span_count == 2);
	CHECK(find_span(&out[0], MD_SPAN_BOLD) >= 0);
	CHECK(find_span(&out[0], MD_SPAN_CODE) >= 0);
	/* Both delimiter pairs are stripped. */
	CHECK(strcmp(out[0].text, "Use bold and code together.") == 0);
}

/* ------------------------------------------------------------------ */
/* Test: output buffer truncation                                      */
/* ------------------------------------------------------------------ */

static void test_truncation(void)
{
	static const char src[] =
		"# H\n"
		"para one\n"
		"para two\n"
		"para three\n"
		"- list\n";
	struct md_block out[3];
	int32_t         n;

	/*
	 * max == 3: parser must stop cleanly without writing past
	 * out[2], returning exactly 3.
	 */
	n = md_parse(src, out, 3);
	CHECK(n == 3);
	/* Verify that the blocks we do have are valid. */
	CHECK(out[0].type == MD_BLOCK_HEADING);
	CHECK(out[1].type == MD_BLOCK_PARAGRAPH);
	CHECK(out[2].type == MD_BLOCK_PARAGRAPH);
}

/* ------------------------------------------------------------------ */
/* Test: text exactly at MD_TEXT_MAX — no overflow                    */
/* ------------------------------------------------------------------ */

static void test_text_cap(void)
{
	/*
	 * Build a line of exactly MD_TEXT_MAX + 10 'a' chars.
	 * The parser must not write past blk.text[MD_TEXT_MAX-1].
	 */
	char           src[MD_TEXT_MAX + 16];
	struct md_block out[4];
	int32_t         n;
	uint32_t        i;

	for (i = 0; i < MD_TEXT_MAX + 10; i++)
		src[i] = 'a';
	src[i++] = '\n';
	src[i]   = '\0';

	n = md_parse(src, out, 4);
	CHECK(n == 1);
	/* NUL must be within the array. */
	CHECK(out[0].text[MD_TEXT_MAX - 1] == '\0');
}

/* ------------------------------------------------------------------ */
/* Test: unterminated bold/code — treated as literal                  */
/* ------------------------------------------------------------------ */

static void test_unterminated_inline(void)
{
	static const char src_bold[] = "No **close here.\n";
	static const char src_code[] = "No `close here.\n";
	struct md_block   out[4];
	int32_t           n;

	n = md_parse(src_bold, out, 4);
	CHECK(n == 1);
	CHECK(find_span(&out[0], MD_SPAN_BOLD) == -1);
	/* Unterminated delimiter stays as literal text. */
	CHECK(strcmp(out[0].text, "No **close here.") == 0);

	n = md_parse(src_code, out, 4);
	CHECK(n == 1);
	CHECK(find_span(&out[0], MD_SPAN_CODE) == -1);
	CHECK(strcmp(out[0].text, "No `close here.") == 0);
}

/* ------------------------------------------------------------------ */
/* Test: heading with no trailing text                                 */
/* ------------------------------------------------------------------ */

static void test_heading_no_text(void)
{
	static const char src[] = "# \n";
	struct md_block   out[4];
	int32_t           n;

	n = md_parse(src, out, 4);
	/* "# " with nothing after — empty heading text is fine. */
	CHECK(n == 1);
	CHECK(out[0].type  == MD_BLOCK_HEADING);
	CHECK(out[0].level == 1);
	CHECK(out[0].text[0] == '\0');
}

/* ------------------------------------------------------------------ */
/* Test: multi-line document — headings, list, paragraph, code        */
/* ------------------------------------------------------------------ */

static void test_full_document(void)
{
	static const char src[] =
		"# Title\n"
		"\n"
		"An intro paragraph with **bold** text.\n"
		"\n"
		"## Section\n"
		"\n"
		"- First item\n"
		"- Second item with `code`\n"
		"\n"
		"```\n"
		"verbatim line\n"
		"```\n"
		"\n"
		"Final paragraph.\n";
	struct md_block out[MD_BLOCKS_MAX];
	int32_t         n;
	int32_t         i;
	int             saw_h1 = 0, saw_h2 = 0, saw_list = 0;
	int             saw_code = 0, saw_para = 0;

	n = md_parse(src, out, MD_BLOCKS_MAX);
	CHECK(n == 7);

	for (i = 0; i < n; i++) {
		switch (out[i].type) {
		case MD_BLOCK_HEADING:
			if (out[i].level == 1)
				saw_h1 = 1;
			if (out[i].level == 2)
				saw_h2 = 1;
			break;
		case MD_BLOCK_LIST_ITEM:
			saw_list = 1;
			break;
		case MD_BLOCK_CODE:
			saw_code = 1;
			break;
		case MD_BLOCK_PARAGRAPH:
			saw_para = 1;
			break;
		}
	}

	CHECK(saw_h1);
	CHECK(saw_h2);
	CHECK(saw_list);
	CHECK(saw_code);
	CHECK(saw_para);
}

/* ------------------------------------------------------------------ */
/* Test: adjacent code fences with no gap                             */
/* ------------------------------------------------------------------ */

static void test_adjacent_fences(void)
{
	static const char src[] =
		"```\n"
		"block one\n"
		"```\n"
		"```\n"
		"block two\n"
		"```\n";
	struct md_block out[8];
	int32_t         n;

	n = md_parse(src, out, 8);
	CHECK(n == 2);
	CHECK(out[0].type == MD_BLOCK_CODE);
	CHECK(out[1].type == MD_BLOCK_CODE);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
	test_empty();
	test_headings();
	test_heading_overflow();
	test_paragraphs();
	test_list_items();
	test_code_fence();
	test_unterminated_fence();
	test_inline_bold();
	test_inline_code();
	test_inline_mixed();
	test_truncation();
	test_text_cap();
	test_unterminated_inline();
	test_heading_no_text();
	test_full_document();
	test_adjacent_fences();

	if (g_failures == 0) {
		printf("All tests passed.\n");
		return 0;
	}
	fprintf(stderr, "%d test(s) FAILED.\n", g_failures);
	return g_failures;
}
