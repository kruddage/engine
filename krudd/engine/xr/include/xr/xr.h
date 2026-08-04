/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef XR_H
#define XR_H

#include <math/math_types.h>

#include <stdint.h>

/*
 * The WebXR session: entering one, leaving one, and what a frame inside one
 * looks like. This module renders NOTHING. It takes the frame loop over for
 * as long as a session runs, turns each XRView the runtime reports into a
 * (view, projection, eye, viewport) entry, and hands the loop back on exit.
 * DRAWING that list is #994's; producing it is this module's whole job.
 *
 * It is also the only place in the engine that knows the word XR, which is
 * the initiative's shape rather than an accident (#987). The three seams it
 * stands on are each about something more general than a headset:
 *
 *   #988  a camera is a (view, proj) pair a caller may supply verbatim,
 *         rather than an eye/target/fov the renderer re-derives a symmetric
 *         projection from — because an off-axis frustum cannot survive being
 *         re-derived.
 *   #990  a "backbuffer" pass draws into a framebuffer and rect the host
 *         names, rather than FBO 0 at the drawing-buffer size.
 *   #991  the rAF loop can be suspended and single-stepped by an external
 *         host, because only an XRSession.requestAnimationFrame callback
 *         receives an XRFrame, and therefore poses.
 *
 * None of those three mentions a session, a headset or an eye. This module is
 * the host that uses all three at once, and no engine module links it back:
 * it reaches down into render/ and base/ and is reached by the page.
 *
 * Every pose and every length here is in metres, which is also the engine's
 * world unit — see the convention stated in math/camera.h. That agreement is
 * what lets a headset pose be consumed unscaled.
 *
 * A session also reports the CONTROLLERS each frame (#996), and there is
 * nothing about that in this header on purpose. A controller's aim is a
 * world-space ray with a click edge, which is not an XR concept and does not
 * want an XR-shaped api: it is composed onto the same stage the eyes are and
 * handed DOWN to ui/viewport's "pointer3d" subsystem, which is where a game
 * reads it (viewport/pointer3d.h) and where it meets the same raycast a canvas
 * click already went through. Nothing below ui/ learns the word XR by it, and
 * this module gains no second public surface for a thing that is not about
 * sessions. The reading itself is xr_input.c and the private xr_bridge.h.
 */

/*
 * How many XRViews a frame's list can hold. Two is the stereo case and what
 * every headset this targets reports, but the count is read per frame rather
 * than assumed: a runtime may report one (a monoscopic view, or a
 * "secondary-view" configuration collapsed to one) or four (a canted
 * quad-view display). A consumer draws the list as long as it actually is.
 */
#define XR_MAX_VIEWS 4

/*
 * Which eye a view belongs to, as WebXR's XRView.eye reports it. "none" is a
 * real answer, not an error — a monoscopic view has no eye — so a consumer
 * that keys off this must handle it rather than assume left/right.
 *
 * The numbers matter: the JS glue reports them by value across the module
 * boundary (see xr_bridge.h), so they are part of this module's contract with
 * itself and may not be renumbered casually.
 */
enum xr_eye {
	XR_EYE_NONE  = 0,
	XR_EYE_LEFT  = 1,
	XR_EYE_RIGHT = 2
};

/*
 * One XRView, in the form the engine's seams consume.
 *
 * view and proj go to camera_set_view_proj() as they are (#988). proj is the
 * runtime's own projectionMatrix, verbatim: it is asymmetric, encoding the
 * interpupillary offset, the lens shift and any panel cant, and none of that
 * can be rebuilt from a field of view and an aspect ratio. It arrives in the
 * GL convention (NDC z in [-1, 1]), which is what camera.h asks a supplied
 * projection to be in and what WebXR produces.
 *
 * view is the WORLD-TO-VIEW matrix — XRView.transform.inverse.matrix — while
 * position is the world-space point that transform puts the eye at. A view
 * matrix no longer implies a camera position once it is supplied rather than
 * derived, and the pbr path uploads one for its view direction, so it is
 * carried explicitly.
 *
 * x, y, width, height are XRWebGLLayer.getViewport(view): a rect inside the
 * layer's framebuffer, in bottom-left-origin framebuffer pixels — the same
 * space webgl_declare_backbuffer() takes (#990). Both eyes usually name one
 * framebuffer and differ only in this rect.
 */
struct xr_view {
	struct mat4 view;
	struct mat4 proj;
	float       position[3];
	int32_t     eye;      /* enum xr_eye */
	int32_t     x;
	int32_t     y;
	int32_t     width;
	int32_t     height;
};

/*
 * The views for the frame currently being drawn, and the framebuffer they
 * are drawn into (XRWebGLLayer.framebuffer, registered as a GL name the
 * backend can bind — see xr_session.c).
 *
 * count is 0 whenever there is no viewer pose to report: outside a session,
 * and inside one on a frame where tracking is lost or the headset is off the
 * user's head. That is not an error and does not stop the frame — the engine
 * still ticks — so a consumer must treat an empty list as "draw nothing for
 * the headset this frame" rather than as "the session ended".
 */
struct xr_view_list {
	uint32_t       fbo;
	int32_t        count;
	struct xr_view views[XR_MAX_VIEWS];
};

/*
 * Whether an immersive-vr session can be entered at all. The probe is
 * asynchronous — navigator.xr.isSessionSupported() returns a promise — so
 * there is a state before the answer arrives, and it is spelled out rather
 * than folded into "no".
 *
 * The three ways it can be "no" are kept apart because they are different
 * facts about the page and a caller may want to say different things about
 * them (#997 puts an affordance on this). NO_API also covers an insecure
 * context: navigator.xr is only exposed on a secure one, so from here the
 * two are indistinguishable, and the log line says so.
 */
enum xr_support {
	XR_SUPPORT_UNKNOWN     = -1,  /* the probe has not answered yet */
	XR_SUPPORT_YES         = 0,
	XR_SUPPORT_NO_API      = 1,   /* no navigator.xr, or not secure */
	XR_SUPPORT_NO_DEVICE   = 2,   /* no immersive-vr on this device */
	XR_SUPPORT_PROBE_ERROR = 3    /* the probe threw or was rejected */
};

/*
 * Ask the browser whether immersive-vr is supported, once. Cheap, safe to
 * call on any page, and the ONLY thing this module does to a page that has no
 * WebXR: it logs one line saying which of the three "no"s it got and changes
 * nothing else. Calling it again while an answer is pending, or after one has
 * arrived, does nothing.
 */
void xr_probe(void);

/* The probe's answer so far. XR_SUPPORT_UNKNOWN until it lands. */
enum xr_support xr_supported(void);

/*
 * Enter an immersive-vr session: request it, make the GL context XR
 * compatible, attach an XRWebGLLayer, acquire a reference space, and take the
 * frame loop over. Every step of that is asynchronous, so this RETURNS
 * IMMEDIATELY and the session is live only once xr_session_active() says so.
 *
 * WebXR requires a user gesture, so this must be called from inside a real
 * event handler — a button's click (#997) — and not from the boot path or a
 * timer. A request that is refused, rejected or made where it cannot succeed
 * leaves the page exactly as it was and logs one line.
 */
void xr_request_session(void);

/*
 * Leave the session. The teardown does not happen here: this asks the browser
 * to end the session, and everything that unwinds the takeover runs from the
 * session's own "end" event. That is deliberate — the user can also end a
 * session from the headset's system menu, which the page never hears about
 * except through that same event — so both exits run one identical path.
 * A no-op when no session is running.
 */
void xr_end_session(void);

/* Nonzero while a session is live and this module is driving the frames. */
int32_t xr_session_active(void);

/*
 * This frame's view list. Never NULL: outside a session it is a valid list
 * with count 0. The storage is this module's own and is refilled in place at
 * the top of every XR frame, so a consumer reads it during the tick it was
 * published for and does not hold the pointer across frames.
 */
const struct xr_view_list *xr_views(void);

#endif /* XR_H */
