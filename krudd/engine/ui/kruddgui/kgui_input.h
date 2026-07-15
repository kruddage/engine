/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef KGUI_INPUT_H
#define KGUI_INPUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * kgui_input — kruddgui's pointer router (the input inversion of #489).
 *
 * kruddgui owns the raw Emscripten pointer callbacks; this module is the
 * GL-free, ImGui-free core that decides, per pointer event, which kruddgui
 * panel (if any) owns it and what the host (ImGui) should still see. It is
 * kept separable from the Emscripten callbacks so the routing logic can be
 * driven by synthetic events in a host test.
 *
 * Model — a per-frame hit-test registry plus a multi-touch state machine:
 *
 *   - Each tick a panel image *declares* its input regions (a named rect per
 *     panel) in draw order; later declarations are on top (z-order). The set
 *     declared this frame is committed at the end of the tick and becomes the
 *     set the async callbacks route against until the next tick — the exact
 *     "built this tick, read by the callbacks between ticks" discipline the v0
 *     single-bar used, generalised to N named regions.
 *
 *   - Each pointer (a touch by its identifier, or the mouse) is tracked
 *     independently. A pointer whose *down* lands on a region is captured by
 *     that region for its whole gesture: its moves accumulate as a drag on the
 *     region and a same-region up registers a tap, none of it forwarded. A
 *     pointer whose down misses every region is forwarded to the host. Only one
 *     unclaimed pointer at a time drives the host (ImGui is single-pointer), so
 *     a second finger on a kruddgui panel never disturbs an ImGui drag, and a
 *     finger on one panel never steals another's.
 *
 * Coordinates are CSS pixels (the space imgui_tick and kgui_batch share); the
 * y axis points down.
 */

#define KGUI_MAX_REGIONS 32
#define KGUI_MAX_TOUCHES 10
#define KGUI_MAX_REGION_IO 32

/*
 * The mouse is routed through the same per-pointer machine as touches, under a
 * reserved identifier that no real touch can collide with.
 */
#define KGUI_MOUSE_ID ((int32_t)0x7fffffff)

/* What the caller (the Emscripten adapter) should do with an event. */
enum kgui_route {
	KGUI_ROUTE_CONSUMED = 0, /* a region owns it — do not forward */
	KGUI_ROUTE_FORWARD  = 1, /* not ours — forward to the host (ImGui) */
};

/*
 * Per-region input results, accumulated by the callbacks between ticks and read
 * by the image when it re-declares the region. `tapped`, `drag_*` and `wheel`
 * describe just the current frame and are cleared at commit; `pressed` tracks
 * the live capture and persists across frames. A slot is keyed by the region's
 * name hash and lives as long as the router does.
 */
struct kgui_region_io {
	uint32_t name;
	int      tapped;            /* a tap completed in the region this frame */
	float    tap_x, tap_y;      /* where the tap landed */
	float    drag_dx, drag_dy;  /* captured pointer motion this frame */
	float    wheel;             /* wheel delta over the region this frame */
	int      pressed;           /* a pointer is currently captured here */
	float    press_x, press_y;  /* live captured-pointer position */
};

struct kgui_region {
	uint32_t name;
	float    x0, y0, x1, y1;
};

struct kgui_touch {
	int      active;
	int32_t  id;        /* touch identifier, or KGUI_MOUSE_ID */
	uint32_t region;    /* captured region name, 0 = unclaimed */
	int      forwarded; /* unclaimed and currently driving the host */
	float    x, y;      /* last position */
};

struct kgui_input {
	struct kgui_region committed[KGUI_MAX_REGIONS];
	int                committed_count;
	struct kgui_region building[KGUI_MAX_REGIONS];
	int                building_count;

	struct kgui_region_io io[KGUI_MAX_REGION_IO];
	int                   io_count;

	struct kgui_touch touches[KGUI_MAX_TOUCHES];
};

/* FNV-1a hash of a region name, folded to a nonzero id (0 means "no region"). */
uint32_t kgui_name_hash(const char *s);

void kgui_input_init(struct kgui_input *in);

/* Begin declaring this frame's regions (clears the building list). */
void kgui_input_frame_begin(struct kgui_input *in);

/*
 * Declare a region for this frame and return its result slot (never NULL) so
 * the caller can read the input accumulated for it since the last commit. The
 * returned pointer is stable until the next kgui_input_region for a new name.
 */
struct kgui_region_io *kgui_input_region(struct kgui_input *in, uint32_t name,
					 float x, float y, float w, float h);

/*
 * Commit the frame: the building list becomes the committed list the callbacks
 * route against, and every slot's per-frame fields (tap, drag, wheel) are
 * cleared so the next frame accumulates afresh.
 */
void kgui_input_frame_commit(struct kgui_input *in);

/*
 * The name of the topmost committed region under (x, y), or 0 if the point is
 * on no panel — the "is the pointer over UI" test a viewport overlay (the
 * transform gizmo) uses to stand down when the pointer is on a panel, the
 * kruddgui equivalent of ImGui's WantCaptureMouse. Reads the committed set, so
 * it reflects the panels declared on the previous tick (a one-frame lag, as
 * WantCaptureMouse had).
 */
uint32_t kgui_input_hit_region(const struct kgui_input *in, float x, float y);

/* Pointer events — driven from the Emscripten callbacks (or a test). */
enum kgui_route kgui_input_pointer_down(struct kgui_input *in, int32_t id,
					float x, float y);
enum kgui_route kgui_input_pointer_move(struct kgui_input *in, int32_t id,
					float x, float y);
enum kgui_route kgui_input_pointer_up(struct kgui_input *in, int32_t id,
				      float x, float y);
enum kgui_route kgui_input_wheel(struct kgui_input *in, float x, float y,
				 float dy);

#ifdef __cplusplus
}
#endif

#endif /* KGUI_INPUT_H */
