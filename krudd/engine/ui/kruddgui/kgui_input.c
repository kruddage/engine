/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * kgui_input — see kgui_input.h. The pointer router: a per-frame region
 * registry and a multi-touch capture machine. No GL, no ImGui, no Emscripten;
 * the callbacks in kruddgui.cpp are a thin adapter over this.
 */

#include "kgui_input.h"

#include <stddef.h>

uint32_t kgui_name_hash(const char *s)
{
	uint32_t h = 2166136261u; /* FNV-1a offset basis */

	if (!s)
		return 1u;
	while (*s) {
		h ^= (uint32_t)(unsigned char)*s++;
		h *= 16777619u;
	}
	return h ? h : 1u; /* 0 is reserved for "no region" */
}

void kgui_input_init(struct kgui_input *in)
{
	int i;

	in->committed_count = 0;
	in->building_count  = 0;
	in->io_count        = 0;
	for (i = 0; i < KGUI_MAX_TOUCHES; i++)
		in->touches[i].active = 0;
}

void kgui_input_frame_begin(struct kgui_input *in)
{
	in->building_count = 0;
}

/* Find the persistent result slot for a name, creating it once on first use. */
static struct kgui_region_io *io_slot(struct kgui_input *in, uint32_t name)
{
	int i;

	for (i = 0; i < in->io_count; i++)
		if (in->io[i].name == name)
			return &in->io[i];

	if (in->io_count >= KGUI_MAX_REGION_IO)
		return &in->io[KGUI_MAX_REGION_IO - 1]; /* clamp, never grow */

	{
		struct kgui_region_io *r = &in->io[in->io_count++];

		r->name    = name;
		r->tapped  = 0;
		r->tap_x   = r->tap_y = 0.0f;
		r->drag_dx = r->drag_dy = 0.0f;
		r->wheel   = 0.0f;
		r->pressed = 0;
		r->press_x = r->press_y = 0.0f;
		return r;
	}
}

struct kgui_region_io *kgui_input_region(struct kgui_input *in, uint32_t name,
					 float x, float y, float w, float h)
{
	if (in->building_count < KGUI_MAX_REGIONS) {
		struct kgui_region *r = &in->building[in->building_count++];

		r->name = name;
		r->x0   = x;
		r->y0   = y;
		r->x1   = x + w;
		r->y1   = y + h;
	}
	return io_slot(in, name);
}

void kgui_input_frame_commit(struct kgui_input *in)
{
	int i;

	for (i = 0; i < in->building_count; i++)
		in->committed[i] = in->building[i];
	in->committed_count = in->building_count;

	/* Per-frame results are consumed; the live `pressed` state carries on. */
	for (i = 0; i < in->io_count; i++) {
		in->io[i].tapped  = 0;
		in->io[i].drag_dx = 0.0f;
		in->io[i].drag_dy = 0.0f;
		in->io[i].wheel   = 0.0f;
	}
}

/* Topmost committed region under a point, or 0 — later declarations win. */
static uint32_t region_at(const struct kgui_input *in, float x, float y)
{
	int i;

	for (i = in->committed_count - 1; i >= 0; i--) {
		const struct kgui_region *r = &in->committed[i];

		if (x >= r->x0 && x <= r->x1 && y >= r->y0 && y <= r->y1)
			return r->name;
	}
	return 0;
}

uint32_t kgui_input_hit_region(const struct kgui_input *in, float x, float y)
{
	return region_at(in, x, y);
}

static const struct kgui_region *region_by_name(const struct kgui_input *in,
						uint32_t name)
{
	int i;

	for (i = 0; i < in->committed_count; i++)
		if (in->committed[i].name == name)
			return &in->committed[i];
	return NULL;
}

static struct kgui_touch *touch_find(struct kgui_input *in, int32_t id)
{
	int i;

	for (i = 0; i < KGUI_MAX_TOUCHES; i++)
		if (in->touches[i].active && in->touches[i].id == id)
			return &in->touches[i];
	return NULL;
}

static struct kgui_touch *touch_alloc(struct kgui_input *in, int32_t id)
{
	int i;

	for (i = 0; i < KGUI_MAX_TOUCHES; i++)
		if (!in->touches[i].active) {
			in->touches[i].active    = 1;
			in->touches[i].id        = id;
			in->touches[i].region    = 0;
			in->touches[i].forwarded = 0;
			return &in->touches[i];
		}
	return NULL;
}

static int any_forwarded(const struct kgui_input *in)
{
	int i;

	for (i = 0; i < KGUI_MAX_TOUCHES; i++)
		if (in->touches[i].active && in->touches[i].forwarded)
			return 1;
	return 0;
}

/* Fold (x, y) into the gesture's peak squared displacement from its down point. */
static void touch_track_move(struct kgui_touch *t, float x, float y)
{
	float ex = x - t->down_x;
	float ey = y - t->down_y;
	float d2 = ex * ex + ey * ey;

	if (d2 > t->moved)
		t->moved = d2;
}

enum kgui_route kgui_input_pointer_down(struct kgui_input *in, int32_t id,
					float x, float y)
{
	struct kgui_touch *t = touch_find(in, id);
	uint32_t           name;

	if (!t)
		t = touch_alloc(in, id);
	if (!t)
		return KGUI_ROUTE_FORWARD; /* out of slots — don't swallow it */

	t->x      = x;
	t->y      = y;
	t->down_x = x;
	t->down_y = y;
	t->moved  = 0.0f;

	name = region_at(in, x, y);
	if (name) {
		struct kgui_region_io *io = io_slot(in, name);

		t->region    = name;
		t->forwarded = 0;
		io->pressed  = 1;
		io->press_x  = x;
		io->press_y  = y;
		return KGUI_ROUTE_CONSUMED;
	}

	/*
	 * Unclaimed: forward to the host only if no other pointer already is —
	 * ImGui is single-pointer, so a second unclaimed finger is swallowed
	 * rather than fighting the first for the mouse.
	 */
	t->region    = 0;
	t->forwarded = !any_forwarded(in);
	return t->forwarded ? KGUI_ROUTE_FORWARD : KGUI_ROUTE_CONSUMED;
}

enum kgui_route kgui_input_pointer_move(struct kgui_input *in, int32_t id,
					float x, float y)
{
	struct kgui_touch *t = touch_find(in, id);
	float              dx, dy;

	/* A move with no tracked down is a hover — the host's to handle. */
	if (!t)
		return KGUI_ROUTE_FORWARD;

	dx   = x - t->x;
	dy   = y - t->y;
	t->x = x;
	t->y = y;
	touch_track_move(t, x, y);

	if (t->region) {
		struct kgui_region_io *io = io_slot(in, t->region);

		io->drag_dx += dx;
		io->drag_dy += dy;
		io->press_x  = x;
		io->press_y  = y;
		return KGUI_ROUTE_CONSUMED;
	}
	return t->forwarded ? KGUI_ROUTE_FORWARD : KGUI_ROUTE_CONSUMED;
}

enum kgui_route kgui_input_pointer_up(struct kgui_input *in, int32_t id,
				      float x, float y)
{
	struct kgui_touch *t = touch_find(in, id);
	int                forwarded;
	uint32_t           region;
	float              moved;

	if (!t)
		return KGUI_ROUTE_FORWARD;

	touch_track_move(t, x, y);
	region    = t->region;
	forwarded = t->forwarded;
	moved     = t->moved;
	t->active = 0; /* release the slot */

	if (region) {
		struct kgui_region_io *io = io_slot(in, region);
		const struct kgui_region *r = region_by_name(in, region);

		io->pressed = 0;
		/*
		 * A tap is a down and an up both inside the same region whose
		 * pointer never wandered past the slop — a gesture that moved
		 * further is a drag (a scroll), and fires no tap so its release
		 * does not open whatever row it lifts over.
		 */
		if (r && x >= r->x0 && x <= r->x1 &&
		    y >= r->y0 && y <= r->y1 &&
		    moved <= KGUI_TAP_SLOP * KGUI_TAP_SLOP) {
			io->tapped = 1;
			io->tap_x  = x;
			io->tap_y  = y;
		}
		return KGUI_ROUTE_CONSUMED;
	}
	return forwarded ? KGUI_ROUTE_FORWARD : KGUI_ROUTE_CONSUMED;
}

enum kgui_route kgui_input_wheel(struct kgui_input *in, float x, float y,
				 float dy)
{
	uint32_t name = region_at(in, x, y);

	if (name) {
		io_slot(in, name)->wheel += dy;
		return KGUI_ROUTE_CONSUMED;
	}
	return KGUI_ROUTE_FORWARD;
}
