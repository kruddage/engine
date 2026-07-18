/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef KRUDDGUI_API_H
#define KRUDDGUI_API_H

#define KRUDDGUI_MAX_OVERLAYS 8

/*
 * kruddgui_api — the seam a C subsystem uses to draw a viewport-space overlay on
 * kruddgui's own quad batch and read its pointer, replacing the ImGui draw list
 * and io the transform gizmo drew against before the migration (#492, feeding
 * #487). The mirror of imgui_api's register_panel, but for kruddgui's owned
 * pipeline.
 *
 * An overlay callback runs each kruddgui tick BEFORE the Scheme panels draw, so
 * the panels compose over it (the gizmo sits under the editor, as it did on
 * ImGui's background draw list). It draws through the vector primitives here —
 * flat-colour lines, rects, discs and rings on the shared batch — in CSS pixels
 * with colours in 0..1.
 *
 * The pointer accessors report the *unclaimed* pointer: the one kruddgui did not
 * route to a panel (the mouse, or the first off-panel finger). pointer_clicked /
 * _released are one-frame edges cleared after the overlays run, the shape of
 * ImGui's IsMouseClicked / IsMouseReleased; over_ui is WantCaptureMouse — true
 * when a point lies on a panel, so the overlay yields the gesture to the UI.
 */
struct kruddgui_api {
	void (*register_overlay)(void (*draw_fn)(void *userdata), void *userdata);

	/* Draw into this tick's batch. */
	void (*line)(float x0, float y0, float x1, float y1, float width,
		     float r, float g, float b, float a);
	void (*rect)(float x, float y, float w, float h,
		     float r, float g, float b, float a);
	void (*circle)(float cx, float cy, float rad,
		       float r, float g, float b, float a);
	void (*ring)(float cx, float cy, float rad, float width,
		     float r, float g, float b, float a);
	/* One line of text with its top-left at (x, y). Returns the advance width. */
	float (*text)(float x, float y, const char *str, float size,
		      float r, float g, float b, float a);

	/* This tick's viewport size in CSS pixels. */
	void (*viewport)(float *w, float *h);

	/* The unclaimed pointer: last position, and button state. */
	void (*pointer)(float *x, float *y);
	int  (*pointer_down)(void);      /* the button is currently held        */
	int  (*pointer_clicked)(void);   /* a press began this frame, off-panel */
	int  (*pointer_released)(void);  /* a release happened this frame       */

	/* True when (x, y) lies on any kruddgui panel — the overlay stands down. */
	int  (*over_ui)(float x, float y);
};

#endif /* KRUDDGUI_API_H */
