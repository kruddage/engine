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
 *
 * The same struct backs both the single-line field and the multiline field
 * (kgui-field-multi): a newline is just an inserted '\n' byte, and the
 * multiline caret helpers below (up / down / line-home / line-end / the
 * line,col,px report) read those newlines to split the buffer into lines. The
 * storage is sized once for both — a shader source (the first multiline
 * customer in PR6h) runs to a few KB, well past a single line's needs — so the
 * cap is 8 KB rather than the ~256 bytes a lone field would want.
 */

#define KGUI_TEXT_EDIT_CAP 8192 /* bytes of storage, including the NUL */

struct kgui_text_edit {
	char  buf[KGUI_TEXT_EDIT_CAP];
	int   len;     /* bytes in buf, excluding the NUL (0..CAP-1) */
	int   caret;   /* caret byte offset, on a code-point boundary (0..len) */
	float goal_px; /* target caret x held across up/down; < 0 = recompute */
};

/*
 * Measure the pixel width of the first `nbytes` of `s` in the caller's font.
 * The multiline caret math is proportional-font aware — a "column" is a pixel
 * x, not a character count — but stays GL-free by taking this callback: the
 * WASM host passes a wrapper over kgui_text_width, the host test a stub. The
 * run is not NUL-terminated; measure exactly `nbytes`.
 */
typedef float (*kgui_text_measure)(const char *s, int nbytes, void *ud);

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

/*
 * Multiline caret helpers. A newline is an ordinary inserted '\n'; these read
 * the newlines to move the caret by visual line rather than by buffer.
 */

/* Jump the caret to the start / end of the line it is on (not the buffer). */
void kgui_text_edit_line_home(struct kgui_text_edit *e);
void kgui_text_edit_line_end(struct kgui_text_edit *e);

/*
 * Move the caret up / down one line, preserving a target pixel-x. The target is
 * the caret's own x when a vertical run begins and is held across consecutive
 * up/down moves (any horizontal edit clears it), so the caret does not drift on
 * a proportional font. In the destination line the caret lands on the code-
 * point boundary whose x is nearest the target. A no-op on the first / last
 * line. `measure` must be non-NULL (there is no px target without it).
 */
void kgui_text_edit_up(struct kgui_text_edit *e,
		       kgui_text_measure measure, void *ud);
void kgui_text_edit_down(struct kgui_text_edit *e,
			 kgui_text_measure measure, void *ud);

/* Number of lines (newlines + 1; always >= 1), for sizing the scroll body. */
int kgui_text_edit_line_count(const struct kgui_text_edit *e);

/*
 * Report the caret's position for drawing: `line` (0-based), `col` (code points
 * since the line start), and `px` (the caret's x within its line, via `measure`
 * — 0 when `measure` is NULL). Any out pointer may be NULL.
 */
void kgui_text_edit_caret(const struct kgui_text_edit *e,
			  kgui_text_measure measure, void *ud,
			  int *line, int *col, float *px);

#ifdef __cplusplus
}
#endif

#endif /* KGUI_TEXT_EDIT_H */
