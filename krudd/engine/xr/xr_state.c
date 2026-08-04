/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "xr_bridge.h"

#include <xr/xr.h>
#include <log/log.h>
#include <math/camera.h>
#include <math/math_types.h>
#include <scene_renderer/scene_view.h>
#include <webgl/renderer_webgl.h>

#ifdef __EMSCRIPTEN__
#include <core/engine.h>

/*
 * kruddgui's overlay-suppression switch (#995). kruddgui sits below xr in
 * kruddmake/manifest.scm's tier order, so xr may reach down to it — but the
 * function is forward-declared here rather than reached through a header
 * because kruddgui exports no public surface for it (its build.scm has no
 * (public ...) clause) and, more to the point, must not gain one shaped like
 * "for xr": the switch is generic ("some external host has claimed the
 * backbuffer"), and kruddgui.cpp says why in its own comment. This is the
 * same cross-module-call shape as scene_renderer_set_view_target below —
 * both modules are wasm-modules of core's index (engine.c), so the
 * definition is in the image without a link edge, and a plain declaration is
 * all a call across that boundary needs. Not called from a native build:
 * xr_test.c's native run never sets __EMSCRIPTEN__, so it never needs a
 * stub for this symbol the way it supplies one for the scene_renderer calls.
 */
void kruddgui_set_overlay_suppressed(int suppressed);
#endif

#include <stdint.h>
#include <string.h>

/*
 * The session state machine and the per-frame view list: everything about
 * being in a session that is not the browser API itself. No emscripten, no
 * EM_JS, no promise — which is what lets a native run have an opinion about
 * it (xr_test.c). The browser half is xr_session.c.
 *
 * Three pieces of state, and they are separate on purpose:
 *
 *   g_support  what the browser said about immersive-vr, once. A fact about
 *              the page, not about a session, and it outlives every session.
 *   g_active   whether a session is running right now. The one flag that
 *              decides whether the frame loop belongs to this module.
 *   g_list     the views for the frame being drawn. Refilled in place at the
 *              top of every XR frame; count 0 whenever there is nothing to
 *              report, which includes the whole time outside a session.
 *
 * It is also where the list becomes a DRAWN frame (#994): the views go to the
 * scene renderer as a scene_view list, and each eye's rect reaches the WebGL
 * backend as that eye is drawn. Both of those are ordinary calls into modules
 * that have never heard of a headset — a list of views (#989) and a host-named
 * backbuffer (#990) — which is what keeps this the only file in the engine
 * that knows a session exists.
 */

static enum xr_support g_support = XR_SUPPORT_UNKNOWN;
static int32_t         g_active;
static struct xr_view_list g_list;

/*
 * The layer framebuffer's full size, as the last published frame reported it.
 * Kept because the per-view target callback has to be able to put the whole
 * layer back after the last eye, and the callback is handed a scene_view — a
 * rect INSIDE the layer — which cannot say how big the layer is.
 */
static uint32_t g_fb_width, g_fb_height;

/*
 * Whether the frame currently being drawn is one this module supplied views
 * for. The renderer calls the target callback for every view it draws,
 * including the single flat view it DERIVES when no list was supplied — a
 * poseless frame inside a live session is exactly that — and a derived view's
 * rect is the canvas's, which has no business being declared against a layer.
 */
static int32_t g_drawing_views;

/*
 * The frame's views on their way in from the glue. Static, so its address
 * never moves: the glue is handed it once, at install time, and keeps it for
 * the life of the page. (The emscripten HEAP* views it writes through DO move
 * when the heap grows, which is why the glue re-reads those every frame and
 * this pointer alone is the stable thing.)
 */
static union xr_stage_slot g_stage[XR_MAX_VIEWS * XR_STAGE_STRIDE];

union xr_stage_slot *xr_stage(void)
{
	return g_stage;
}

enum xr_support xr_supported(void)
{
	return g_support;
}

int32_t xr_session_active(void)
{
	return g_active;
}

const struct xr_view_list *xr_views(void)
{
	return &g_list;
}

/*
 * The one line a page with no headset gets out of this module. Written as
 * whole sentences per case rather than a code, because this is the common
 * case — most pages that ever run this are a desktop browser — and "which
 * kind of no" is the only thing anyone reading the console wants to know.
 *
 * navigator.xr is exposed on secure contexts only, so from inside the page a
 * missing API and an insecure origin are the same observation, and the line
 * says both rather than guessing one.
 */
static const char *support_line(enum xr_support s)
{
	switch (s) {
	case XR_SUPPORT_YES:
		return "immersive-vr is supported";
	case XR_SUPPORT_NO_API:
		return "no navigator.xr — this browser has no WebXR, or the "
		       "page is not a secure context";
	case XR_SUPPORT_NO_DEVICE:
		return "navigator.xr is here but immersive-vr is not "
		       "supported — no headset on this machine";
	case XR_SUPPORT_PROBE_ERROR:
		return "the immersive-vr support probe failed — blocked by "
		       "permissions policy, most likely";
	default:
		return "support is unknown";
	}
}

void xr_report_support(int32_t support)
{
	enum xr_support s = (enum xr_support)support;

	/*
	 * The probe answers once. A second answer would mean the glue asked
	 * twice, and logging it again would turn one clear line into noise on
	 * every call — so the first one stands.
	 */
	if (g_support != XR_SUPPORT_UNKNOWN)
		return;

	g_support = s;
	if (s == XR_SUPPORT_YES)
		LOG_INFO("xr: %s", support_line(s));
	else
		LOG_INFO("xr: %s; the page is unchanged", support_line(s));
}

/*
 * Where this view lands inside the layer framebuffer, said to the WebGL
 * backend as the renderer reaches the view (#990, #994). This is the whole of
 * how a per-eye rect gets out of this module: the renderer carries the rect in
 * the scene_view it was given and hands it back here, and nothing between the
 * two knows it is an eye.
 *
 * VIEW == NULL is the end of the frame's views, and the layer goes back to
 * being one target: whatever draws after the scene — the UI compositing over
 * it — then lands across the whole layer rather than inside the right eye. Not
 * a tidy-up, a correctness rule; the declaration must not outlive the view it
 * was made for.
 *
 * Silent unless this module supplied the frame's views. A frame with no viewer
 * pose leaves the renderer on its derived flat view, whose rect is the canvas's
 * and means nothing here, so the whole-layer declaration xr_frame_publish left
 * stands and that frame goes out stretched across the layer — which is what a
 * session showed before this PR, and is the right thing to show for the frame
 * or two before tracking arrives.
 */
static void xr_view_target(const struct scene_view *view, void *user)
{
	(void)user;

	if (!g_active || !g_drawing_views || !g_fb_width || !g_fb_height)
		return;
	if (!view) {
		webgl_declare_backbuffer(g_list.fbo, 0, 0,
					 g_fb_width, g_fb_height);
		return;
	}
	webgl_declare_backbuffer(g_list.fbo, view->target_x, view->target_y,
				 view->width, view->height);
}

void xr_session_begun(void)
{
	if (g_active)
		return;

	g_active = 1;
	g_list.count = 0;
	xr_input_reset();

	/*
	 * Take the per-view target callback for as long as the session runs
	 * (#989's seam, #990's declaration). Installed here rather than at the
	 * first published frame so that there is exactly one place it is taken
	 * and exactly one place it is given back, matching the loop handover
	 * below — a callback left installed after a session would point every
	 * flat frame at a framebuffer that no longer exists.
	 */
	scene_renderer_set_view_target(xr_view_target, NULL);

	/*
	 * The takeover (#991). From here until the session ends, engine_tick
	 * runs from an XRSession.requestAnimationFrame callback and from
	 * nothing else — because that callback is the only place an XRFrame
	 * exists, and the XRFrame is the only source of a pose. Leaving the
	 * window rAF running alongside it would not just waste a frame: it
	 * would draw one with no pose to draw it from, into a layer
	 * framebuffer that is only valid inside the XR callback.
	 *
	 * Paired here rather than in the glue so that the flag above is what
	 * decides. Every path out of a session — the page's own exit, the
	 * headset's system menu, a setup that failed halfway — lands in
	 * xr_session_ended(), and the flag makes the resume happen exactly
	 * once for each suspend.
	 */
#ifdef __EMSCRIPTEN__
	krudd_suspend_loop();

	/*
	 * Off (#995). kruddgui's screen-space overlay composites flat, at
	 * zero depth, over whatever the backbuffer currently is; for the
	 * length of a session that backbuffer is the layer framebuffer this
	 * module just took over for stereo (xr_frame_publish,
	 * webgl_declare_backbuffer above), and the same flat composite over
	 * that is pasted across both eyes rather than existing anywhere in
	 * the scene. Paired with the loop takeover for the same reason:
	 * both are "kruddgui's tick still runs, but not this part of it"
	 * for as long as the headset owns the frame.
	 */
	kruddgui_set_overlay_suppressed(1);
#endif
	LOG_INFO("xr: session started; the frame loop is the headset's");
}

/*
 * Why the session is over, as a line rather than a code. XR_END_NORMAL is
 * the ordinary exit AND the system-menu one — the page cannot tell them
 * apart, and does not need to, because the browser reports both as the
 * session's "end" event.
 */
static const char *end_line(int32_t reason)
{
	switch (reason) {
	case XR_END_REFUSED:
		return "the session request was refused";
	case XR_END_SETUP:
		return "the session could not be set up";
	default:
		return "session ended";
	}
}

void xr_session_ended(int32_t reason)
{
	/*
	 * Nothing to unwind. This is not a misuse: a request that never
	 * became a session reports its failure here, and so does a second
	 * "end" for a session that already ended. Both want the line and
	 * neither wants the teardown.
	 */
	if (!g_active) {
		if (reason != XR_END_NORMAL)
			LOG_WARN("xr: %s; the page is unchanged",
				 end_line(reason));
		return;
	}

	g_active = 0;
	g_list.count = 0;
	g_list.fbo = 0;
	g_fb_width = 0;
	g_fb_height = 0;
	g_drawing_views = 0;

	/*
	 * The controllers go with the eyes (#996). A pointer left published
	 * after the session would leave a debug ray hanging in the flat page's
	 * scene, aimed from a hand that is no longer in the room.
	 */
	xr_input_reset();

	/*
	 * Hand the canvas back. The declaration named a framebuffer that
	 * belonged to the session's layer and stops existing with it, so
	 * leaving it in place would point every backbuffer pass on the flat
	 * page at a framebuffer that is gone (#990). Undeclared is what the
	 * page booted in and what it returns to: FBO 0, full drawing buffer.
	 */
	webgl_clear_backbuffer();

	/*
	 * And hand the renderer back to the camera (#989, #994). The last
	 * frame's eyes are two head poses that stopped being true the moment
	 * the user took the headset off, and a list outlives the frame it was
	 * supplied for by design — so leaving it would draw the flat page twice
	 * per frame, from a pose nobody is standing in, into rects of a
	 * framebuffer that is gone. Both halves go together: the list and the
	 * callback that was translating it.
	 */
	scene_renderer_clear_views();
	scene_renderer_set_view_target(NULL, NULL);

#ifdef __EMSCRIPTEN__
	/*
	 * Back on (#995), before the loop hands back: the very next tick
	 * after this one runs from window rAF again, over the canvas
	 * backbuffer webgl_clear_backbuffer just restored, and it must find
	 * kruddgui drawing exactly as it did before the session — every
	 * entry into a session paired with the exit that undoes it is what
	 * "repeatedly entering and exiting leaves the flat UI correct every
	 * time" means in practice.
	 */
	kruddgui_set_overlay_suppressed(0);
	krudd_resume_loop();
#endif
	LOG_INFO("xr: %s; the flat page has the frame loop back",
		 end_line(reason));
}

void xr_stage_offset(float out[3])
{
	struct camera scene_cam;
	int32_t       i;

	for (i = 0; i < 3; i++)
		out[i] = 0.0f;
	if (!scene_renderer_scene_camera(&scene_cam))
		return;
	for (i = 0; i < 3; i++)
		out[i] = scene_cam.position[i];
}

static void unpack_view(struct xr_view *out, const union xr_stage_slot *s)
{
	int32_t i;

	for (i = 0; i < 16; i++) {
		out->view.m[i] = s[XR_STAGE_VIEW + i].f;
		out->proj.m[i] = s[XR_STAGE_PROJ + i].f;
	}
	for (i = 0; i < 3; i++)
		out->position[i] = s[XR_STAGE_POS + i].f;

	out->eye    = s[XR_STAGE_EYE].i;
	out->x      = s[XR_STAGE_X].i;
	out->y      = s[XR_STAGE_Y].i;
	out->width  = s[XR_STAGE_WIDTH].i;
	out->height = s[XR_STAGE_HEIGHT].i;
}

/*
 * WebXR's own defaults for depthNear / depthFar, which are what a session gets
 * when there is no renderer to ask — a page whose scene has not initialised
 * yet. Repeated here rather than left implicit because the whole point of
 * xr_depth_range is that the two ends agree, and "whatever the browser picks"
 * is not something this side can assert against.
 */
#define XR_DEFAULT_DEPTH_NEAR 0.1f
#define XR_DEFAULT_DEPTH_FAR  1000.0f

void xr_depth_range(float *near, float *far)
{
	struct camera cam;

	if (!near || !far)
		return;
	*near = XR_DEFAULT_DEPTH_NEAR;
	*far  = XR_DEFAULT_DEPTH_FAR;

	if (!scene_renderer_scene_camera(&cam))
		return;
	/*
	 * A range the runtime would refuse is not passed on. updateRenderState
	 * throws on a non-positive or inverted range, and a throw there loses
	 * the layer along with it — so a renderer reporting something the
	 * authored path could never produce costs the frame's depth precision
	 * rather than the session.
	 */
	if (cam.near > 0.0f && cam.far > cam.near) {
		*near = cam.near;
		*far  = cam.far;
	}
}

/*
 * One XRView, composed onto the stage and handed to the renderer as a view it
 * can draw (#988's supplied pair, #989's list entry).
 *
 * STAGE is where the scene's own camera is, and it is added to the pose rather
 * than replacing it: WebXR reports poses about the user's floor, so consumed
 * raw they stand a viewer at the world origin no matter where the scene framed
 * itself. Translation only — the reasoning, which is a statement about what the
 * scripted camera means, is written where that camera is declared
 * (scene_renderer.c, g_camera_entity_id).
 *
 * The pose is a translation of the world, so it is applied to the world:
 * view' = view * translate(-STAGE), which leaves the rotation — every term the
 * user's head contributes — untouched, and moves the eye's world position by
 * +STAGE. Nothing rescales, reorders or re-derives the projection; it is copied
 * across as the runtime built it, because an off-axis frustum that has been
 * adjusted no longer matches the lens it was measured for.
 *
 * eye/target/up are filled as well as the pair, and they are not decoration:
 * the cosmetic particle billboards take their screen basis from them
 * (scene_view.h says so, and a view that leaves them zeroed silently draws no
 * particles). They come from this eye's own view matrix — row 0 is its right,
 * row 1 its up, row 2 its backward — so a billboard faces the eye that is
 * looking at it rather than some average of the two.
 */
static void compose_view(struct scene_view *out, const struct xr_view *v,
			 const float stage[3], float near, float far)
{
	struct mat4 offset, view;
	float       world[3], up[3], target[3];
	int32_t     i;

	memset(out, 0, sizeof(*out));

	mat4_identity(&offset);
	offset.m[12] = -stage[0];
	offset.m[13] = -stage[1];
	offset.m[14] = -stage[2];
	mat4_mul(&view, &v->view, &offset);

	for (i = 0; i < 3; i++)
		world[i] = v->position[i] + stage[i];

	up[0] = view.m[1];
	up[1] = view.m[5];
	up[2] = view.m[9];
	/* Row 2 of a view matrix is the camera's BACKWARD in world space (GL
	 * cameras look down -Z), so the look target is a step against it. */
	target[0] = world[0] - view.m[2];
	target[1] = world[1] - view.m[6];
	target[2] = world[2] - view.m[10];

	for (i = 0; i < 3; i++) {
		out->cam.eye[i]    = world[i];
		out->cam.target[i] = target[i];
		out->cam.up[i]     = up[i];
	}
	/*
	 * The range this projection was built over, carried so the view says
	 * what it was framed for. Nothing reads it while a pair is supplied —
	 * camera_update leaves such a camera alone — but a view whose near/far
	 * disagreed with the frustum it carries would be a trap for the first
	 * reader that ever wanted them.
	 */
	out->cam.near = near;
	out->cam.far  = far;

	camera_set_view_proj(&out->cam, &view, &v->proj, world);

	out->width    = (uint32_t)v->width;
	out->height   = (uint32_t)v->height;
	out->target_x = v->x;
	out->target_y = v->y;
}

/*
 * Hand the frame's views to the renderer, composed onto the scene's camera.
 * Returns how many were supplied, which is not always g_list.count: a runtime
 * may report a view it does not want drawn, and XRWebGLLayer.getViewport()
 * says so by handing back no rect at all (the glue stages that as a zero-sized
 * one). Drawing it would cost a whole graph build and a whole scene draw to
 * produce nothing, and would declare a rect with no area for the backend to
 * scissor every clear to — so those views are dropped rather than drawn. The
 * loop also stops at SCENE_MAX_VIEWS, which is the renderer's bound and is not
 * this module's to assume equals XR_MAX_VIEWS.
 *
 * WHAT THIS COSTS, PLAINLY. Every view here is a full trip through the frame
 * graph: shadow map, forward pass, selection mask, the four-pass bloom chain
 * and the composite, at that eye's resolution. Two eyes is therefore twice all
 * of it, at a headset's resolution and a headset's refresh, and the shadow map
 * in particular is drawn twice for a result that does not depend on which eye
 * is looking (#989 says why it is a graph transient and therefore cannot simply
 * be hoisted). Nothing here has been measured on a headset, and no number is
 * claimed for it: this repo's build cannot even reach a device. What can be
 * said honestly is the shape — 2x the whole graph, with the shadow pass and the
 * emissive walk the two known-redundant parts — and which knobs exist if it
 * does not hold frame rate: drop bloom while a session runs, hoist the shadow
 * pass out of the per-view loop (which needs one graph spanning the views), or
 * OVR_multiview2, which draws both eyes in one pass and is the real answer.
 * #987 lists all three as follow-ups and rules them out of this PR, and none of
 * them is worth choosing before someone has a frame time from a real device.
 */
static int32_t publish_scene_views(void)
{
	struct scene_view views[SCENE_MAX_VIEWS];
	float             stage[3];
	float             near, far;
	int32_t           i, n = 0;

	xr_depth_range(&near, &far);
	/*
	 * The scene's camera as the last tick left it — this runs from the XR
	 * animation frame, one call before the tick it is publishing for, so
	 * the stage is a frame behind the scripted camera whenever that camera
	 * is itself moving. That is not head-pose latency, which is the kind
	 * that makes people ill: the head poses being composed here arrived
	 * moments ago in this very callback and are drawn in this very frame.
	 * It is the scene sliding one frame late under a viewer while the scene
	 * itself is in motion, and the only way to close it would be to move
	 * the composition inside the renderer's own tick — which would put
	 * "there is a stage" into a module that must not learn it (#987).
	 *
	 * Read through xr_stage_offset so that the controller rays published
	 * this same frame (#996) compose onto the identical translation. Two
	 * copies of "where the scene put the viewer" that could disagree is a
	 * ray that points somewhere the user is not looking.
	 */
	xr_stage_offset(stage);

	for (i = 0; i < g_list.count && n < SCENE_MAX_VIEWS; i++) {
		if (g_list.views[i].width <= 0 || g_list.views[i].height <= 0)
			continue;
		compose_view(&views[n], &g_list.views[i], stage, near, far);
		n++;
	}

	if (n > 0)
		scene_renderer_set_views(views, (uint32_t)n);
	else
		scene_renderer_clear_views();
	return n;
}

void xr_frame_publish(uint32_t fbo, uint32_t fb_width, uint32_t fb_height,
		      int32_t count)
{
	int32_t i;

	/*
	 * A frame with no layer to draw into. The glue reports the layer's
	 * size, and zero means it had none to report: a session whose render
	 * state has not landed yet, or one whose layer the runtime replaced
	 * between frames. Publish nothing and put the backbuffer back on the
	 * canvas, rather than declaring a zero-sized rect on a framebuffer
	 * that is not there — a target with no area is not a target, and a
	 * declared one would silently swallow the frame.
	 */
	if (fb_width == 0 || fb_height == 0) {
		g_list.fbo      = 0;
		g_list.count    = 0;
		g_fb_width      = 0;
		g_fb_height     = 0;
		g_drawing_views = 0;
		webgl_clear_backbuffer();
		scene_renderer_clear_views();
		return;
	}

	/*
	 * count is the one number here a runtime chose rather than this
	 * module, so it is clamped rather than trusted. A runtime reporting
	 * more views than the list holds gets its first XR_MAX_VIEWS drawn
	 * instead of overrunning; silently, because this runs every frame and
	 * a warning here would be a warning sixty times a second.
	 */
	if (count < 0)
		count = 0;
	if (count > XR_MAX_VIEWS)
		count = XR_MAX_VIEWS;

	g_list.fbo   = fbo;
	g_list.count = count;
	for (i = 0; i < count; i++)
		unpack_view(&g_list.views[i], g_stage + i * XR_STAGE_STRIDE);

	g_fb_width  = fb_width;
	g_fb_height = fb_height;

	/*
	 * Name the layer's framebuffer as the backbuffer for this frame
	 * (#990). Re-declared every frame rather than once at session start:
	 * the layer can be replaced under a running session — an
	 * updateRenderState() with a new framebuffer scale, a runtime that
	 * reallocates on a visibility change — and the framebuffer name it
	 * reports is the only place that shows.
	 *
	 * Over the WHOLE layer, which is where the frame starts and ends. Each
	 * eye narrows it to its own rect as the renderer reaches that eye, and
	 * xr_view_target puts this back once the last one is drawn.
	 */
	webgl_declare_backbuffer(fbo, 0, 0, fb_width, fb_height);

	/*
	 * And the same views to the renderer, as views it can draw (#994). This
	 * is the last thing publishing does, after the list and the declaration
	 * are both settled, because composing them reads the list and the
	 * callback it arms reads the declaration.
	 */
	g_drawing_views = publish_scene_views();
}
