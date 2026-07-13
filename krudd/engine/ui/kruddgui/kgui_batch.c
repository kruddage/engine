/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * kgui_batch — see kgui_batch.h. Pure geometry: builds the vertex batch a
 * kruddgui panel flushes as one draw call. No GL, no ImGui.
 */

#include "kgui_batch.h"

#include <stddef.h>

void kgui_batch_init(struct kgui_batch *b, struct kgui_vertex *storage, int cap)
{
	b->verts    = storage;
	b->cap      = cap;
	b->count    = 0;
	b->overflow = 0;
	kgui_batch_reset(b);
}

void kgui_batch_reset(struct kgui_batch *b)
{
	b->count     = 0;
	b->overflow  = 0;
	b->cmd_count = 0;
	b->clip_on   = 0;
	b->clip_tex  = 0;
}

void kgui_batch_set_clip(struct kgui_batch *b,
			 float x, float y, float w, float h)
{
	b->clip_on = 1;
	b->clip_x  = x;
	b->clip_y  = y;
	b->clip_w  = w;
	b->clip_h  = h;
}

void kgui_batch_clear_clip(struct kgui_batch *b)
{
	b->clip_on = 0;
}

/*
 * The draw command that the next quad belongs to: the last one when its clip and
 * texture match the current ones, otherwise a fresh command starting at the
 * current vertex. When the command list is full the last command is reused so
 * vertices are never dropped for want of a command (its clip/texture may then be
 * stale, a bounded and unlikely overflow the renderer tolerates).
 */
static struct kgui_clip_cmd *cur_cmd(struct kgui_batch *b)
{
	struct kgui_clip_cmd *c;

	if (b->cmd_count > 0) {
		c = &b->cmds[b->cmd_count - 1];
		if (c->tex == b->clip_tex && c->clipped == b->clip_on &&
		    (!b->clip_on ||
		     (c->x == b->clip_x && c->y == b->clip_y &&
		      c->w == b->clip_w && c->h == b->clip_h)))
			return c;
	}

	if (b->cmd_count >= KGUI_BATCH_MAX_CMDS)
		return &b->cmds[KGUI_BATCH_MAX_CMDS - 1];

	c          = &b->cmds[b->cmd_count++];
	c->clipped = b->clip_on;
	c->x       = b->clip_x;
	c->y       = b->clip_y;
	c->w       = b->clip_w;
	c->h       = b->clip_h;
	c->tex     = b->clip_tex;
	c->first   = b->count;
	c->count   = 0;
	return c;
}

static void push_vertex(struct kgui_batch *b, float x, float y,
			float u, float v, float r, float g, float bl, float a)
{
	struct kgui_vertex *out;

	if (b->count >= b->cap) {
		b->overflow = 1;
		return;
	}
	out    = &b->verts[b->count++];
	out->x = x;
	out->y = y;
	out->u = u;
	out->v = v;
	out->r = r;
	out->g = g;
	out->b = bl;
	out->a = a;
}

/*
 * Push one quad (two triangles) sampling texture `tex` — 0 for the atlas path
 * (rects and glyphs), an external handle for an image. The texture is staged on
 * the batch so cur_cmd opens or extends the matching command before the vertices
 * land.
 */
static void emit_quad(struct kgui_batch *b,
		      float x, float y, float w, float h,
		      float u0, float v0, float u1, float v1,
		      unsigned tex, float r, float g, float bl, float a)
{
	float                 x1  = x + w;
	float                 y1  = y + h;
	struct kgui_clip_cmd *cmd;

	b->clip_tex = tex;
	cmd         = cur_cmd(b);

	/*
	 * Two triangles, counter-clockwise, sharing the tl->br diagonal.
	 * A six-vertex list keeps each clip run a single non-indexed draw.
	 */
	push_vertex(b, x,  y,  u0, v0, r, g, bl, a);
	push_vertex(b, x1, y,  u1, v0, r, g, bl, a);
	push_vertex(b, x1, y1, u1, v1, r, g, bl, a);

	push_vertex(b, x,  y,  u0, v0, r, g, bl, a);
	push_vertex(b, x1, y1, u1, v1, r, g, bl, a);
	push_vertex(b, x,  y1, u0, v1, r, g, bl, a);

	/* Extend the run to whatever actually landed (short on overflow). */
	cmd->count = b->count - cmd->first;
}

void kgui_batch_quad(struct kgui_batch *b,
		     float x, float y, float w, float h,
		     float u0, float v0, float u1, float v1,
		     float r, float g, float bl, float a)
{
	emit_quad(b, x, y, w, h, u0, v0, u1, v1, 0u, r, g, bl, a);
}

void kgui_batch_image(struct kgui_batch *b,
		      float x, float y, float w, float h,
		      float u0, float v0, float u1, float v1,
		      unsigned tex,
		      float r, float g, float bl, float a)
{
	emit_quad(b, x, y, w, h, u0, v0, u1, v1, tex, r, g, bl, a);
}

uint32_t kgui_utf8_next(const char **s)
{
	const unsigned char *p = (const unsigned char *)*s;
	uint32_t             c = p[0];
	int                  n = 0;
	int                  i;

	if (c < 0x80) {
		n = 0;
	} else if ((c & 0xE0) == 0xC0) {
		c &= 0x1F;
		n  = 1;
	} else if ((c & 0xF0) == 0xE0) {
		c &= 0x0F;
		n  = 2;
	} else if ((c & 0xF8) == 0xF0) {
		c &= 0x07;
		n  = 3;
	} else {
		/* Stray continuation or invalid lead: pass the byte through. */
		*s = (const char *)(p + 1);
		return p[0];
	}

	for (i = 0; i < n; i++) {
		if ((p[1 + i] & 0xC0) != 0x80) {
			/* Truncated sequence: consume the lead byte only. */
			*s = (const char *)(p + 1);
			return p[0];
		}
		c = (c << 6) | (p[1 + i] & 0x3F);
	}
	*s = (const char *)(p + 1 + n);
	return c;
}

float kgui_batch_text(struct kgui_batch *b, float x, float y,
		      const char *str, float size,
		      float r, float g, float bl, float a,
		      kgui_glyph_fn glyphs, void *ud)
{
	const char *p   = str;
	float       pen = x;

	if (!str)
		return 0.0f;

	while (*p) {
		uint32_t          cp = kgui_utf8_next(&p);
		struct kgui_glyph gl;

		if (!glyphs(ud, cp, size, &gl))
			continue;
		if (b && gl.visible)
			kgui_batch_quad(b, pen + gl.x0, y + gl.y0,
					gl.x1 - gl.x0, gl.y1 - gl.y0,
					gl.u0, gl.v0, gl.u1, gl.v1,
					r, g, bl, a);
		pen += gl.advance;
	}
	return pen - x;
}

float kgui_text_width(const char *str, float size,
		      kgui_glyph_fn glyphs, void *ud)
{
	return kgui_batch_text(NULL, 0.0f, 0.0f, str, size,
			       0.0f, 0.0f, 0.0f, 0.0f, glyphs, ud);
}
