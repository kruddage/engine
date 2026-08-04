/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SCENE_VIEW_H
#define SCENE_VIEW_H

#include <math/camera.h>

#include <stdint.h>

/*
 * The views a frame draws (#989).
 *
 * scene_renderer_tick used to build exactly one frame graph per tick, against
 * one global camera and one viewport size. That is one view per frame by
 * construction, and it is the assumption anything drawing the same world more
 * than once in a frame runs into: a split screen, a portal, an editor preview
 * beside the main viewport — and a headset, which wants the world drawn twice
 * from two poses through two projections into two rects of one target.
 *
 * So a frame draws a LIST of views instead, one after another, each with its
 * own graph. The list is length one for the flat page, derived from the
 * camera and the reported viewport exactly as before, and with one view the
 * emitted passes are the passes the renderer emitted before this existed.
 * Nothing in this build supplies a longer one.
 *
 * A view is a camera plus the size of what it draws into. It is a whole
 * struct camera rather than a bare (view, proj, eye) triple because that is
 * what struct camera already is after #988 — a pair and the point it was
 * built about, with eye/target/fov as one producer of that pair — and because
 * the renderer still reads the producer in one place: the cosmetic particle
 * billboards take their screen basis from eye/target/up (see draw_particles),
 * not from the view matrix, so that an authored view's particles come out
 * bit-for-bit as they did before the list existed. A producer that supplies a
 * pair of its own should therefore fill eye/target/up as well if the scene
 * has particles to orient; one that leaves them zeroed gets a degenerate
 * basis and no particles, which is the safe direction to fail in and is
 * flagged here rather than discovered.
 *
 * WHAT IS NOT HERE, deliberately: the view's offset within a shared target.
 * width/height size this view's own transients (the offscreen scene colour and
 * depth, the MSAA resolve, the bloom half-res chain, the outline texel), which
 * is every dimension the renderer itself chooses. WHERE the result lands inside
 * a target the renderer did not create is the host's business and is declared
 * to the backend by the host — that is exactly what #990's
 * webgl_declare_backbuffer(fbo, x, y, w, h) is for. The frame graph's
 * import_backbuffer names the whole target and takes no rect, so an x/y here
 * would be a field with no reader and no way to acquire one; #994, which owns
 * both halves of that wiring, is where it belongs if it is ever needed.
 */

/*
 * Views a single frame may carry. Two is stereo, which is the case this exists
 * for; four leaves room for a runtime that reports more (quad-view headsets
 * report one pair per panel) without the cap being the thing that stops it.
 * Every view costs a full graph build and a full scene draw, so this is a
 * sanity bound, not a budget — see the uniform-ring note in scene_renderer.c
 * for what N views actually spend.
 */
#define SCENE_MAX_VIEWS 4

struct scene_view {
	/*
	 * The camera this view draws with: the (view, proj) pair every pass
	 * uploads, the world-space eye a shader's cam_pos reads, and the
	 * authored eye/target/up the particle billboards orient against.
	 */
	struct camera cam;
	/*
	 * The pixel size this view renders at. Drives every transient the
	 * frame graph declares for it, so two views at different sizes get
	 * their own targets rather than sharing one sized from a global.
	 * Zero on either axis means "no viewport reported yet", which puts
	 * this view on the direct-to-backbuffer fallback with no post chain.
	 */
	uint32_t      width;
	uint32_t      height;
};

/*
 * Draw these views, in this order, every frame until the list is cleared.
 * COUNT above SCENE_MAX_VIEWS is refused outright (the list is left as it
 * was) rather than truncated: a caller that asked for six eyes and silently
 * got four would draw a wrong frame with no way to notice.
 *
 * The list is per-frame data — a headset's poses change every frame — so a
 * caller re-supplies it ahead of each tick, the way it re-supplies a pose.
 * Passing count 0 is the same as clearing.
 *
 * NOTHING IN THIS BUILD CALLS THIS. The flat page derives its single view
 * from the camera and the viewport, and does so precisely because no list was
 * supplied. This is the seam #993/#994 drive; it is public so a sibling module
 * can reach it and so the two-view path is exercisable at all today.
 */
void scene_renderer_set_views(const struct scene_view *views, uint32_t count);

/*
 * Hand the renderer back to the single view it derives from the camera. Safe
 * to call when no list was supplied.
 */
void scene_renderer_clear_views(void);

#endif /* SCENE_VIEW_H */
