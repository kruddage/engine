/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * kgui_text_edit — see kgui_text_edit.h. Pure UTF-8 string surgery for the
 * soft-keyboard field: no GL, no ImGui, no Emscripten.
 */

#include "kgui_text_edit.h"

#include <string.h>

/* Bytes in the UTF-8 sequence led by `c` (1 for a continuation/ASCII byte). */
static int lead_len(unsigned char c)
{
	if (c < 0x80)
		return 1;
	if ((c & 0xE0) == 0xC0)
		return 2;
	if ((c & 0xF0) == 0xE0)
		return 3;
	if ((c & 0xF8) == 0xF0)
		return 4;
	return 1; /* stray continuation byte: treat as one */
}

/* Byte offset of the code-point boundary just before `i` (i must be > 0). */
static int prev_boundary(const struct kgui_text_edit *e, int i)
{
	i--;
	while (i > 0 && ((unsigned char)e->buf[i] & 0xC0) == 0x80)
		i--;
	return i;
}

/* Start-of-line byte offset for position `pos` (just after the previous '\n'). */
static int line_start_at(const struct kgui_text_edit *e, int pos)
{
	while (pos > 0 && e->buf[pos - 1] != '\n')
		pos--;
	return pos;
}

/* End-of-line byte offset for position `pos` (the next '\n', or the buffer end). */
static int line_end_at(const struct kgui_text_edit *e, int pos)
{
	while (pos < e->len && e->buf[pos] != '\n')
		pos++;
	return pos;
}

/*
 * Byte offset of the code-point boundary in [start, end] whose pixel x from
 * `start` is nearest `goal`. Walks the line one code point at a time, measuring
 * the run start..boundary; the caret snaps to the closest boundary so a
 * proportional font's columns line up by pixel, not by character.
 */
static int nearest_boundary_px(const struct kgui_text_edit *e, int start,
			       int end, float goal, kgui_text_measure measure,
			       void *ud)
{
	int   best = start;
	float best_d = goal < 0.0f ? -goal : goal; /* distance at start (x = 0) */
	int   i = start;

	while (i < end) {
		float px, d;

		i += lead_len((unsigned char)e->buf[i]);
		if (i > end)
			i = end;
		px = measure(e->buf + start, i - start, ud);
		d  = px - goal;
		if (d < 0.0f)
			d = -d;
		if (d < best_d) {
			best_d = d;
			best   = i;
		}
	}
	return best;
}

void kgui_text_edit_clear(struct kgui_text_edit *e)
{
	e->buf[0]  = '\0';
	e->len     = 0;
	e->caret   = 0;
	e->goal_px = -1.0f;
}

void kgui_text_edit_set(struct kgui_text_edit *e, const char *s)
{
	int n;

	kgui_text_edit_clear(e);
	if (!s)
		return;
	n = (int)strlen(s);
	if (n > KGUI_TEXT_EDIT_CAP - 1)
		n = KGUI_TEXT_EDIT_CAP - 1;
	/* Trim a truncation that would split a trailing multi-byte sequence. */
	while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80)
		n--;
	memcpy(e->buf, s, (size_t)n);
	e->buf[n] = '\0';
	e->len    = n;
	e->caret  = n;
}

int kgui_text_edit_insert(struct kgui_text_edit *e, const char *utf8, int nbytes)
{
	int put = 0;
	int i   = 0;

	if (!utf8)
		return 0;
	while (i < nbytes && utf8[i]) {
		int cl = lead_len((unsigned char)utf8[i]);

		if (i + cl > nbytes)
			break; /* truncated trailing sequence */
		if (e->len + cl > KGUI_TEXT_EDIT_CAP - 1)
			break; /* no room for this whole code point */

		/* Open a gap at the caret and drop the code point in. */
		memmove(e->buf + e->caret + cl, e->buf + e->caret,
			(size_t)(e->len - e->caret));
		memcpy(e->buf + e->caret, utf8 + i, (size_t)cl);
		e->len   += cl;
		e->caret += cl;
		put      += cl;
		i        += cl;
	}
	e->buf[e->len] = '\0';
	e->goal_px = -1.0f;
	return put;
}

void kgui_text_edit_backspace(struct kgui_text_edit *e)
{
	int p;

	if (e->caret <= 0)
		return;
	p = prev_boundary(e, e->caret);
	memmove(e->buf + p, e->buf + e->caret, (size_t)(e->len - e->caret));
	e->len  -= e->caret - p;
	e->caret = p;
	e->buf[e->len] = '\0';
	e->goal_px = -1.0f;
}

void kgui_text_edit_delete_fwd(struct kgui_text_edit *e)
{
	int cl;

	if (e->caret >= e->len)
		return;
	cl = lead_len((unsigned char)e->buf[e->caret]);
	if (e->caret + cl > e->len)
		cl = e->len - e->caret;
	memmove(e->buf + e->caret, e->buf + e->caret + cl,
		(size_t)(e->len - e->caret - cl));
	e->len -= cl;
	e->buf[e->len] = '\0';
	e->goal_px = -1.0f;
}

void kgui_text_edit_left(struct kgui_text_edit *e)
{
	if (e->caret > 0)
		e->caret = prev_boundary(e, e->caret);
	e->goal_px = -1.0f;
}

void kgui_text_edit_right(struct kgui_text_edit *e)
{
	if (e->caret < e->len) {
		e->caret += lead_len((unsigned char)e->buf[e->caret]);
		if (e->caret > e->len)
			e->caret = e->len;
	}
	e->goal_px = -1.0f;
}

void kgui_text_edit_home(struct kgui_text_edit *e)
{
	e->caret   = 0;
	e->goal_px = -1.0f;
}

void kgui_text_edit_end(struct kgui_text_edit *e)
{
	e->caret   = e->len;
	e->goal_px = -1.0f;
}

void kgui_text_edit_line_home(struct kgui_text_edit *e)
{
	e->caret   = line_start_at(e, e->caret);
	e->goal_px = -1.0f;
}

void kgui_text_edit_line_end(struct kgui_text_edit *e)
{
	e->caret   = line_end_at(e, e->caret);
	e->goal_px = -1.0f;
}

void kgui_text_edit_up(struct kgui_text_edit *e,
		       kgui_text_measure measure, void *ud)
{
	int cur_start = line_start_at(e, e->caret);
	int prev_start;

	if (!measure || cur_start == 0)
		return; /* no font, or already on the first line */
	if (e->goal_px < 0.0f)
		e->goal_px = measure(e->buf + cur_start,
				     e->caret - cur_start, ud);
	/* cur_start - 1 is the '\n' ending the previous line. */
	prev_start = line_start_at(e, cur_start - 1);
	e->caret   = nearest_boundary_px(e, prev_start, cur_start - 1,
					 e->goal_px, measure, ud);
}

void kgui_text_edit_down(struct kgui_text_edit *e,
			 kgui_text_measure measure, void *ud)
{
	int cur_start = line_start_at(e, e->caret);
	int cur_end   = line_end_at(e, e->caret);
	int next_start, next_end;

	if (!measure || cur_end >= e->len)
		return; /* no font, or already on the last line */
	if (e->goal_px < 0.0f)
		e->goal_px = measure(e->buf + cur_start,
				     e->caret - cur_start, ud);
	next_start = cur_end + 1; /* step over the '\n' */
	next_end   = line_end_at(e, next_start);
	e->caret   = nearest_boundary_px(e, next_start, next_end,
					 e->goal_px, measure, ud);
}

int kgui_text_edit_line_count(const struct kgui_text_edit *e)
{
	int i, n = 1;

	for (i = 0; i < e->len; i++)
		if (e->buf[i] == '\n')
			n++;
	return n;
}

void kgui_text_edit_caret(const struct kgui_text_edit *e,
			  kgui_text_measure measure, void *ud,
			  int *line, int *col, float *px)
{
	int start = line_start_at(e, e->caret);
	int i, ln = 0, co = 0;

	if (line) {
		for (i = 0; i < start; i++)
			if (e->buf[i] == '\n')
				ln++;
		*line = ln;
	}
	if (col) {
		for (i = start; i < e->caret; i += lead_len((unsigned char)e->buf[i]))
			co++;
		*col = co;
	}
	if (px)
		*px = measure ? measure(e->buf + start, e->caret - start, ud)
			      : 0.0f;
}
