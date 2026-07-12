/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef KGUI_BATCH_H
#define KGUI_BATCH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * kgui_batch — the GL-free geometry core of kruddgui's 2D quad renderer.
 *
 * kruddgui draws its panels as a single triangle batch: filled rectangles and
 * font-atlas glyph quads, all in one vertex buffer, flushed in one draw call.
 * The geometry that goes into that buffer is built here, with no GL and no
 * ImGui dependency, so it can be unit-tested on the host without a context —
 * the GL upload/draw and the ImGui font atlas live in kruddgui.cpp.
 *
 * Coordinates are CSS pixels (the space imgui_tick projects into); the y axis
 * points down. Colours are straight (non-premultiplied) RGBA in 0..1.
 */

struct kgui_vertex {
	float x, y;       /* position, CSS pixels */
	float u, v;       /* atlas texture coords */
	float r, g, b, a; /* colour, 0..1          */
};

/*
 * A fixed-capacity vertex batch over caller-owned storage. `overflow` latches
 * when a push would exceed `cap` so the renderer can size its buffer up; the
 * batch never writes past `storage[cap]`.
 */
struct kgui_batch {
	struct kgui_vertex *verts;
	int                 cap;
	int                 count;
	int                 overflow;
};

void kgui_batch_init(struct kgui_batch *b, struct kgui_vertex *storage,
		     int cap);
void kgui_batch_reset(struct kgui_batch *b);

/*
 * Push one axis-aligned quad as two triangles (6 vertices) with the given
 * UV rectangle and a single colour. A filled rectangle is this quad pointed at
 * the atlas's white pixel (u0==u1, v0==v1).
 */
void kgui_batch_quad(struct kgui_batch *b,
		     float x, float y, float w, float h,
		     float u0, float v0, float u1, float v1,
		     float r, float g, float bl, float a);

/*
 * One glyph as reported by a font source: a pen-relative box and its atlas UVs.
 * `visible` is 0 for a glyph with no drawable area (e.g. space), which emits no
 * quad but still advances the pen.
 */
struct kgui_glyph {
	float x0, y0, x1, y1; /* offset box relative to the pen origin, px */
	float u0, v0, u1, v1; /* atlas texture coords                     */
	float advance;        /* pen advance, px                          */
	int   visible;
};

/*
 * Resolve one Unicode code point at a pixel size into a glyph. Returns 0 when
 * the font has no glyph for it (the caller skips it). The void* is opaque
 * userdata — in kruddgui it is the ImGui font; the test passes a fake.
 */
typedef int (*kgui_glyph_fn)(void *ud, uint32_t cp, float size,
			     struct kgui_glyph *out);

/*
 * Emit quads for a UTF-8 string with its top-left at (x, y), at pixel size
 * `size`, in one colour. Returns the total advance width in pixels.
 */
float kgui_batch_text(struct kgui_batch *b, float x, float y,
		      const char *str, float size,
		      float r, float g, float bl, float a,
		      kgui_glyph_fn glyphs, void *ud);

/* Advance width of a UTF-8 string at `size`, without emitting geometry. */
float kgui_text_width(const char *str, float size,
		      kgui_glyph_fn glyphs, void *ud);

/*
 * Decode one UTF-8 code point from `s`, advancing *`s` past it. Malformed bytes
 * decode as the byte value so a bad string still terminates. Exposed for the
 * test; kruddgui uses it via kgui_batch_text.
 */
uint32_t kgui_utf8_next(const char **s);

#ifdef __cplusplus
}
#endif

#endif /* KGUI_BATCH_H */
