/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef KGUI_TEXT_EDIT_H
#define KGUI_TEXT_EDIT_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * kgui_text_edit — the GL-free editable text buffer behind kruddgui's soft-
 * keyboard field.
 *
 * ImGui had no touch-keyboard model; kruddgui's field owns focus and a caret
 * and is fed by the hidden-<input> bridge (krudd_text_input_*). This module is
 * just the string surgery — insert a run of typed UTF-8 at the caret, backspace
 * / delete a whole code point, and walk the caret by code points — with no GL,
 * no ImGui and no Emscripten, so it is unit-tested on the host. kruddgui.cpp
 * wraps it with focus, the char/key drain, and the drawn caret.
 *
 * The buffer is a NUL-terminated UTF-8 string in fixed storage; every edit
 * keeps it NUL-terminated and the caret on a code-point boundary. Bytes are
 * never split mid-sequence: an insert that would not fit whole is dropped, and
 * backspace / delete / the caret moves step over whole code points.
 */

#define KGUI_TEXT_EDIT_CAP 256 /* bytes of storage, including the NUL */

struct kgui_text_edit {
	char buf[KGUI_TEXT_EDIT_CAP];
	int  len;   /* bytes in buf, excluding the NUL (0..CAP-1) */
	int  caret; /* caret byte offset, on a code-point boundary (0..len) */
};

/* Empty the buffer and park the caret at the start. */
void kgui_text_edit_clear(struct kgui_text_edit *e);

/* Seed the buffer from a C string (truncated to fit) with the caret at the end. */
void kgui_text_edit_set(struct kgui_text_edit *e, const char *s);

/*
 * Insert up to `nbytes` of UTF-8 at the caret, code point by code point, and
 * advance the caret past what landed. A code point that would overflow the
 * storage is dropped (and stops the run). Returns the number of bytes inserted.
 */
int kgui_text_edit_insert(struct kgui_text_edit *e, const char *utf8, int nbytes);

/* Delete the code point before the caret (no-op at the start). */
void kgui_text_edit_backspace(struct kgui_text_edit *e);

/* Delete the code point at the caret (no-op at the end). */
void kgui_text_edit_delete_fwd(struct kgui_text_edit *e);

/* Walk the caret one code point left / right (clamped to the buffer). */
void kgui_text_edit_left(struct kgui_text_edit *e);
void kgui_text_edit_right(struct kgui_text_edit *e);

/* Jump the caret to the start / end of the buffer. */
void kgui_text_edit_home(struct kgui_text_edit *e);
void kgui_text_edit_end(struct kgui_text_edit *e);

#ifdef __cplusplus
}
#endif

#endif /* KGUI_TEXT_EDIT_H */
