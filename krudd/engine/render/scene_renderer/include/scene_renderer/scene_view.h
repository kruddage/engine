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
 * emitted passes are the passes the renderer emitted before this existed. The
 * one thing in this build that supplies a longer list is the WebXR session
 * module, which supplies a pair (#994); every other frame the page draws is
 * still the derived one.
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
 * THE VIEW'S OFFSET WITHIN A SHARED TARGET is here now, and #989 said it would
 * only arrive with a reader (#994 is that reader). width/height size this
 * view's own transients — the offscreen scene colour and depth, the MSAA
 * resolve, the bloom half-res chain, the outline texel — which is every
 * dimension the renderer itself chooses. WHERE the result lands inside a target
 * the renderer did not create is still not something the renderer can act on:
 * the frame graph's import_backbuffer names the whole target and takes no rect,
 * and no backend-neutral call exists to narrow it (#990 argued an external
 * framebuffer name has nothing to say to WebGPU's surface-texture model, so it
 * is that backend's own surface rather than a gpu_api entry).
 *
 * So the rect travels, and the host acts on it. target_x/target_y complete the
 * rect that width/height start, and scene_renderer_set_view_target() installs
 * the one callback the renderer makes per view: "this is the view I am about to
 * draw". A host answers it by pointing its backend at the rect — for the WebGL
 * backend, webgl_declare_backbuffer(fbo, x, y, w, h) — and the renderer never
 * learns what the answer meant, which is what keeps two eyes in one framebuffer
 * out of this file's vocabulary.
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
	/*
	 * Where this view's (width, height) rect sits inside a target the
	 * renderer did not create, in that target's own pixel space — for a
	 * GL-side target that is the bottom-left-origin framebuffer pixels
	 * webgl_declare_backbuffer and cmd_set_scissor already speak.
	 *
	 * The renderer never reads these: they exist to be handed straight back
	 * out through the per-view target callback below, because the renderer
	 * has no backend-neutral way to narrow a backbuffer to a rect and the
	 * host it got the list from does. Zero is the whole point of a target
	 * with one view in it, which is every list this build's flat page draws.
	 */
	int32_t       target_x;
	int32_t       target_y;
};

/*
 * "I am about to draw this view." Called once per view, immediately before its
 * graph is built, and once more with VIEW == NULL after the last view of the
 * frame has been drawn and destroyed.
 *
 * The NULL call is not a formality. A host that answered the per-view calls by
 * narrowing its backbuffer to each view's rect has, by the end of the loop,
 * left that narrowing pointing at the LAST view — and whatever draws after the
 * scene in the same frame (a UI compositing over it, say) would land inside one
 * eye. So the frame's views end as explicitly as they begin, and a host puts
 * its target back where a non-view pass should find it.
 *
 * The callback runs inside the tick, between graph builds. It must not set the
 * view list, must not tick the renderer, and must not create or destroy GPU
 * resources; declaring where the next pass draws is the whole of what it is
 * for.
 */
typedef void (*scene_view_target_fn)(const struct scene_view *view, void *user);

/*
 * Install (or, with FN == NULL, remove) the per-view target callback. USER is
 * handed back unread.
 *
 * There is one, not one per view, because a host that owns a shared target owns
 * all of it: the thing that differs between views is the rect, and that rides
 * in the view. The flat page installs none — it draws one view into the whole
 * of FBO 0 and has no rect to declare — and absent a callback the renderer
 * behaves exactly as it did before this existed.
 */
void scene_renderer_set_view_target(scene_view_target_fn fn, void *user);

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
 * THE FLAT PAGE NEVER CALLS THIS, and that is what keeps it the frame it always
 * was: it derives its single view from the camera and the viewport precisely
 * because no list was supplied. The one caller is the WebXR session module,
 * which supplies the frame's eyes from inside a session and clears the list on
 * the way out (#993, #994) — a sibling module reaching a public surface, which
 * is why this is public and why nothing generic had to learn the word.
 */
void scene_renderer_set_views(const struct scene_view *views, uint32_t count);

/*
 * Hand the renderer back to the single view it derives from the camera. Safe
 * to call when no list was supplied.
 */
void scene_renderer_clear_views(void);

/*
 * The scene's own camera, copied out: the pose the scene authored (and the
 * scripted "Camera" entity drives), and the depth range it authored with.
 * Returns nonzero when there is one to report, zero — leaving OUT untouched —
 * before the renderer has initialised.
 *
 * This is the camera the frame DERIVES its single view from, not the view it
 * draws: supply a list and the renderer draws that instead, while this keeps
 * answering what the scene itself said. That is exactly what a host supplying
 * views needs, and it needs it for two different reasons:
 *
 *   - the scene's pose is the only statement a scene makes about where it is
 *     meant to be viewed from, so a host placing viewers of its own has
 *     somewhere to place them relative to;
 *   - near/far are the range every authored projection in this build is
 *     constructed over, so a host BUILDING its own projections (or asking a
 *     runtime to build them) has the range the scene was framed for rather
 *     than a number it invented.
 *
 * A copy, not a pointer: the renderer rebuilds this camera every tick, and a
 * caller reading it across one would be reading a matrix mid-write.
 */
int32_t scene_renderer_scene_camera(struct camera *out);

#endif /* SCENE_VIEW_H */
