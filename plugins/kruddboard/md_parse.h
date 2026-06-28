/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MD_PARSE_H
#define MD_PARSE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * md_parse — dependency-free markdown parser.
 *
 * Parses a NUL-terminated markdown string into a flat array of
 * md_block structs.  Each block carries a type, an integer level
 * (meaningful for headings), a plain-text body, and a parallel
 * array of inline style runs that overlay the body text.
 *
 * Caller allocates the output arrays; the parser never calls malloc.
 * This mirrors the asset_api.describe(out, max) convention: pass
 * caller-owned buffers with a max count and receive back the number
 * of entries actually written.
 *
 * Supported syntax
 * ----------------
 *   # / ## / ###          headings (level 1–3)
 *   - text / * text       unordered list items
 *   ``` ... ```           fenced code blocks (verbatim)
 *   **word**              inline bold
 *   `word`                inline code
 *   plain text            paragraph blocks
 *
 * Limits
 * ------
 *   MD_TEXT_MAX           maximum body length per block (bytes incl. NUL)
 *   MD_SPANS_PER_BLOCK    maximum inline-style runs per block
 *   MD_BLOCKS_MAX         suggested maximum for a single parse call
 */

#define MD_TEXT_MAX        256
#define MD_SPANS_PER_BLOCK 16
#define MD_BLOCKS_MAX      128

/* Block type discriminator. */
#define MD_BLOCK_PARAGRAPH 0
#define MD_BLOCK_HEADING   1
#define MD_BLOCK_LIST_ITEM 2
#define MD_BLOCK_CODE      3

/* Inline style flags (combinable). */
#define MD_SPAN_NORMAL 0
#define MD_SPAN_BOLD   1
#define MD_SPAN_CODE   2

/*
 * One inline-styled run within a block's body text.
 * [start, end) are byte offsets into md_block.text.
 */
struct md_span {
	uint32_t start; /* byte offset of first char in run */
	uint32_t end;   /* byte offset one past last char   */
	uint32_t style; /* MD_SPAN_* flags                  */
};

/*
 * One parsed block.  text is always NUL-terminated.
 * spans[0..span_count) cover styled sub-ranges of text; any byte
 * not covered by a span is implicitly MD_SPAN_NORMAL.
 */
struct md_block {
	int32_t        type;       /* MD_BLOCK_* */
	int32_t        level;      /* heading level 1–3; 0 for others */
	char           text[MD_TEXT_MAX];
	struct md_span spans[MD_SPANS_PER_BLOCK];
	uint32_t       span_count;
};

/*
 * md_parse — parse src into out[0..max).
 *
 * Parameters
 *   src   NUL-terminated markdown source; must not be NULL.
 *   out   caller-allocated array of at least max md_block structs.
 *   max   capacity of out[]; parse stops when full (no overflow).
 *
 * Returns the number of blocks written to out[], in [0, max].
 * If max == 0 or src is empty, returns 0 immediately.
 */
int32_t md_parse(const char *src, struct md_block *out, int32_t max);

#ifdef __cplusplus
}
#endif

#endif /* MD_PARSE_H */
