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

void kgui_text_edit_clear(struct kgui_text_edit *e)
{
	e->buf[0] = '\0';
	e->len    = 0;
	e->caret  = 0;
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
}

void kgui_text_edit_left(struct kgui_text_edit *e)
{
	if (e->caret > 0)
		e->caret = prev_boundary(e, e->caret);
}

void kgui_text_edit_right(struct kgui_text_edit *e)
{
	if (e->caret < e->len) {
		e->caret += lead_len((unsigned char)e->buf[e->caret]);
		if (e->caret > e->len)
			e->caret = e->len;
	}
}

void kgui_text_edit_home(struct kgui_text_edit *e)
{
	e->caret = 0;
}

void kgui_text_edit_end(struct kgui_text_edit *e)
{
	e->caret = e->len;
}
