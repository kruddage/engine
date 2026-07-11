/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * md_parse.c — dependency-free markdown block parser.
 *
 * Design
 * ------
 * The parser is a single-pass line scanner.  It classifies each line
 * into one of four block types, copies the relevant text into the
 * caller-supplied md_block buffer, then runs an inline span pass over
 * that text to tag **bold** and `code` regions.
 *
 * No dynamic allocation is performed.  All intermediate state lives
 * on the C stack or in the caller-owned output array.
 *
 * Fenced code blocks
 * ------------------
 * Opening ``` consumes lines verbatim until a closing ``` line or
 * end-of-input.  Each interior line becomes its own MD_BLOCK_CODE
 * block (level == 0).  An unterminated fence is silently closed at
 * EOF — the caller gets whatever lines were accumulated.
 *
 * Inline spans
 * ------------
 * Bold  : **text** — must open and close on the same line.
 * Code  : `text`  — must open and close on the same line.
 * The delimiters are stripped from the block text; each span covers the
 * inner run so md_draw renders it styled without the literal ** / ` markers.
 * Spans that would overflow MD_SPANS_PER_BLOCK are silently dropped;
 * the plain text is still emitted correctly.
 */

#include "md_parse.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */
/* ------------------------------------------------------------------ */

/*
 * copy_n — copy at most n bytes from src into dst, always NUL-terminate.
 * Returns number of bytes copied (excluding NUL).
 */
static uint32_t copy_n(char *dst, const char *src, uint32_t n, uint32_t cap)
{
	uint32_t limit = (n < cap - 1) ? n : cap - 1;
	uint32_t i;

	for (i = 0; i < limit; i++)
		dst[i] = src[i];
	dst[i] = '\0';
	return i;
}

/*
 * add_span — append a span to blk if capacity allows.
 */
static void add_span(struct md_block *blk, uint32_t start,
		     uint32_t end, uint32_t style)
{
	struct md_span *sp;

	if (blk->span_count >= MD_SPANS_PER_BLOCK)
		return;
	if (start >= end)
		return;
	sp        = &blk->spans[blk->span_count++];
	sp->start = start;
	sp->end   = end;
	sp->style = style;
}

/*
 * parse_inline — rewrite blk->text with the **bold** / `code` delimiters
 * removed, emitting a span over each styled run's now-delimiter-free text.
 * Plain (unstyled) regions are not emitted; the caller treats any byte not
 * covered by a span as MD_SPAN_NORMAL.  Stripping the delimiters here keeps
 * them out of what md_draw renders — otherwise the literal ** / ` markers
 * would show up in the output.  An unterminated delimiter is left in place
 * as literal text with no span.
 */
static void parse_inline(struct md_block *blk)
{
	const char *t   = blk->text;
	uint32_t    len = (uint32_t)strlen(t);
	char        out[MD_TEXT_MAX];
	uint32_t    o   = 0; /* write cursor into out[] */
	uint32_t    i   = 0;

	while (i < len && o < MD_TEXT_MAX - 1) {
		if (t[i] == '*' && i + 1 < len && t[i + 1] == '*') {
			/* Bold: **...** — copy the inner run, drop the **. */
			uint32_t open = i + 2;
			uint32_t j;

			for (j = open; j + 1 < len; j++) {
				if (t[j] == '*' && t[j + 1] == '*')
					break;
			}
			if (j + 1 < len) {
				uint32_t start = o;

				while (open < j && o < MD_TEXT_MAX - 1)
					out[o++] = t[open++];
				add_span(blk, start, o, MD_SPAN_BOLD);
				i = j + 2;
				continue;
			}
			/* No closing ** found; keep the char as literal. */
			out[o++] = t[i++];
		} else if (t[i] == '`') {
			/* Inline code: `...` — copy the inner run, drop the `. */
			uint32_t open = i + 1;
			uint32_t j;

			for (j = open; j < len; j++) {
				if (t[j] == '`')
					break;
			}
			if (j < len) {
				uint32_t start = o;

				while (open < j && o < MD_TEXT_MAX - 1)
					out[o++] = t[open++];
				add_span(blk, start, o, MD_SPAN_CODE);
				i = j + 1;
				continue;
			}
			/* No closing ` found; keep the char as literal. */
			out[o++] = t[i++];
		} else {
			out[o++] = t[i++];
		}
	}
	out[o] = '\0';
	memcpy(blk->text, out, (size_t)o + 1);
}

/*
 * init_block — zero-initialise *blk.
 */
static void init_block(struct md_block *blk)
{
	blk->type       = MD_BLOCK_PARAGRAPH;
	blk->level      = 0;
	memset(blk->text, 0, sizeof(blk->text));
	blk->span_count = 0;
}

/*
 * emit_block — finalise blk and advance the output cursor.
 * Returns 1 if the block was emitted, 0 if out was full.
 */
static int emit_block(struct md_block *out, int32_t *count,
		      int32_t max, struct md_block *blk)
{
	if (*count >= max)
		return 0;
	out[*count] = *blk;
	(*count)++;
	return 1;
}

/* ------------------------------------------------------------------ */
/* Line helpers                                                        */
/* ------------------------------------------------------------------ */

/*
 * skip_spaces — advance *p past ' ' and '\t'.
 */
static void skip_spaces(const char **p)
{
	while (**p == ' ' || **p == '\t')
		(*p)++;
}

/*
 * line_len — length of current line not including '\n' or '\0'.
 */
static uint32_t line_len(const char *p)
{
	uint32_t n = 0;

	while (p[n] != '\n' && p[n] != '\0')
		n++;
	return n;
}

/*
 * is_fence — returns 1 if line starts with ```.
 */
static int is_fence(const char *p)
{
	return p[0] == '`' && p[1] == '`' && p[2] == '`';
}

/*
 * count_hashes — count leading '#' chars; returns 0 if not a heading
 * or if level > 3 or no space follows.
 */
static int count_hashes(const char *p)
{
	int n = 0;

	while (p[n] == '#')
		n++;
	if (n == 0 || n > 3)
		return 0;
	if (p[n] != ' ' && p[n] != '\t')
		return 0;
	return n;
}

/*
 * is_list_marker — returns 1 if line starts with "- " or "* ".
 */
static int is_list_marker(const char *p)
{
	return (p[0] == '-' || p[0] == '*') && (p[1] == ' ' || p[1] == '\t');
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int32_t md_parse(const char *src, struct md_block *out, int32_t max)
{
	const char    *p     = src;
	int32_t        count = 0;
	int            in_fence;
	struct md_block blk;

	if (!src || !out || max <= 0)
		return 0;

	in_fence = 0;

	while (*p != '\0' && count < max) {
		uint32_t    len  = line_len(p);
		const char *line = p;

		/* Advance p past this line (and '\n' if present). */
		p += len;
		if (*p == '\n')
			p++;

		/* ---- Fenced code block handling ---- */
		if (in_fence) {
			if (is_fence(line)) {
				in_fence = 0;
			} else {
				/* Emit each interior line as a code block. */
				init_block(&blk);
				blk.type = MD_BLOCK_CODE;
				copy_n(blk.text, line, len, MD_TEXT_MAX);
				emit_block(out, &count, max, &blk);
			}
			continue;
		}

		if (is_fence(line)) {
			in_fence = 1;
			continue;
		}

		/* ---- Skip blank lines ---- */
		{
			const char *tmp = line;

			skip_spaces(&tmp);
			if ((uint32_t)(tmp - line) == len) {
				/* Blank or whitespace-only line. */
				continue;
			}
		}

		/* ---- Classify non-fence lines ---- */
		init_block(&blk);

		{
			int hashes = count_hashes(line);

			if (hashes > 0) {
				/* Heading */
				const char *text = line + hashes;

				skip_spaces(&text);
				blk.type  = MD_BLOCK_HEADING;
				blk.level = hashes;
				copy_n(blk.text, text,
				       len - (uint32_t)(text - line),
				       MD_TEXT_MAX);
				parse_inline(&blk);
				emit_block(out, &count, max, &blk);
				continue;
			}
		}

		if (is_list_marker(line)) {
			const char *text = line + 2;

			blk.type  = MD_BLOCK_LIST_ITEM;
			blk.level = 0;
			copy_n(blk.text, text,
			       len - (uint32_t)(text - line),
			       MD_TEXT_MAX);
			parse_inline(&blk);
			emit_block(out, &count, max, &blk);
			continue;
		}

		/* Paragraph */
		blk.type  = MD_BLOCK_PARAGRAPH;
		blk.level = 0;
		copy_n(blk.text, line, len, MD_TEXT_MAX);
		parse_inline(&blk);
		emit_block(out, &count, max, &blk);
	}

	return count;
}
