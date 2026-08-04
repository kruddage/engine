/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "xr_bridge.h"

#include <xr/xr.h>
#include <log/log.h>
#include <webgl/renderer_webgl.h>

#ifdef __EMSCRIPTEN__
#include <core/engine.h>
#endif

#include <stdint.h>

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
 */

static enum xr_support g_support = XR_SUPPORT_UNKNOWN;
static int32_t         g_active;
static struct xr_view_list g_list;

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

void xr_session_begun(void)
{
	if (g_active)
		return;

	g_active = 1;
	g_list.count = 0;

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

	/*
	 * Hand the canvas back. The declaration named a framebuffer that
	 * belonged to the session's layer and stops existing with it, so
	 * leaving it in place would point every backbuffer pass on the flat
	 * page at a framebuffer that is gone (#990). Undeclared is what the
	 * page booted in and what it returns to: FBO 0, full drawing buffer.
	 */
	webgl_clear_backbuffer();

#ifdef __EMSCRIPTEN__
	krudd_resume_loop();
#endif
	LOG_INFO("xr: %s; the flat page has the frame loop back",
		 end_line(reason));
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
		g_list.fbo   = 0;
		g_list.count = 0;
		webgl_clear_backbuffer();
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

	/*
	 * Name the layer's framebuffer as the backbuffer for this frame
	 * (#990). Re-declared every frame rather than once at session start:
	 * the layer can be replaced under a running session — an
	 * updateRenderState() with a new framebuffer scale, a runtime that
	 * reallocates on a visibility change — and the framebuffer name it
	 * reports is the only place that shows.
	 */
	webgl_declare_backbuffer(fbo, 0, 0, fb_width, fb_height);
}
