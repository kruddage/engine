/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * md_parse_scm.c — the C face of the Scheme markdown parser.
 *
 * This is the strangler-fig shim for the first C-to-Scheme port. md_parse.scm
 * holds the parser; this file keeps the md_parse.h ABI intact by loading that
 * image into the shared s7 runtime once, calling (md-parse src), and marshaling
 * the returned block list back into the caller's struct md_block[]. Nothing
 * about the parser lives here — only the string-in, structure-out bridge, the
 * one reusable piece every future port will lean on.
 *
 * A block from Scheme is (type level text spans); a span is (start end style).
 */
#include "md_parse.h"

#include "script.h"

#include "s7.h"

#include "md_parse_scm.h"   /* MD_PARSE_SCM — md_parse.scm, baked in by krudd */

#include <string.h>

/*
 * Return the interpreter with md_parse.scm loaded into it, or NULL if it could
 * not be started. The image is evaluated once; s7 is process-global so the
 * (md-parse ...) definition persists across calls.
 */
static s7_scheme *md_ensure(void)
{
	static int  loaded;
	s7_scheme  *s7 = script_s7();

	if (s7 && !loaded) {
		script_eval(MD_PARSE_SCM);
		loaded = 1;
	}
	return s7;
}

/* Marshal one Scheme block (type level text spans) into *b. */
static void md_marshal_block(s7_scheme *s7, s7_pointer blk, struct md_block *b)
{
	s7_pointer text, spans, sp;

	b->type       = MD_BLOCK_PARAGRAPH;
	b->level      = 0;
	b->text[0]    = '\0';
	b->span_count = 0;

	if (!s7_is_pair(blk))
		return;

	b->type  = (int32_t)s7_integer(s7_list_ref(s7, blk, 0));
	b->level = (int32_t)s7_integer(s7_list_ref(s7, blk, 1));

	text = s7_list_ref(s7, blk, 2);
	if (s7_is_string(text)) {
		const char *t = s7_string(text);
		uint32_t    n = (uint32_t)strlen(t);

		if (n > MD_TEXT_MAX - 1)
			n = MD_TEXT_MAX - 1;
		memcpy(b->text, t, n);
		b->text[n] = '\0';
	}

	spans = s7_list_ref(s7, blk, 3);
	while (s7_is_pair(spans) && b->span_count < MD_SPANS_PER_BLOCK) {
		sp = s7_car(spans);
		if (s7_is_pair(sp)) {
			struct md_span *d = &b->spans[b->span_count++];

			d->start = (uint32_t)s7_integer(s7_list_ref(s7, sp, 0));
			d->end   = (uint32_t)s7_integer(s7_list_ref(s7, sp, 1));
			d->style = (uint32_t)s7_integer(s7_list_ref(s7, sp, 2));
		}
		spans = s7_cdr(spans);
	}
}

int32_t md_parse(const char *src, struct md_block *out, int32_t max)
{
	s7_scheme *s7;
	s7_pointer proc, res;
	int32_t    count = 0;

	if (!src || !out || max <= 0)
		return 0;

	s7 = md_ensure();
	if (!s7)
		return 0;

	proc = s7_name_to_value(s7, "md-parse");
	if (!s7_is_procedure(proc))
		return 0;

	/*
	 * (md-parse src) returns the whole block list; taking the first `max`
	 * yields the same prefix the C parser, which stops scanning at `max`,
	 * would have produced. No s7 allocation happens while walking the
	 * result, so it needs no GC protection.
	 */
	res = s7_call(s7, proc, s7_list(s7, 1, s7_make_string(s7, src)));

	while (count < max && s7_is_pair(res)) {
		md_marshal_block(s7, s7_car(res), &out[count]);
		count++;
		res = s7_cdr(res);
	}
	return count;
}
