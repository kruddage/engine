/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef XR_BRIDGE_H
#define XR_BRIDGE_H

#include <xr/xr.h>

#include <stdint.h>

/*
 * The inward half of this module: what the browser-side glue (xr_session.c's
 * EM_JS) calls back into C, and the state it drives. Private, because nothing
 * outside this module has any business synthesising a session — but not
 * hidden from the module's own test, which drives this seam in the glue's
 * place. That is the whole reason it is a seam at all: the state machine and
 * the view-list bookkeeping are plain C with no browser in them, so they are
 * pinned down natively, the way renderer_webgl_test pins the declare-
 * backbuffer bookkeeping down without a GL context.
 *
 * What CANNOT be tested that way is the glue itself — the promise chain, the
 * XR animation frame, the layer — which needs a real browser and a real
 * headset. Keeping the two halves in separate translation units is what
 * decides which of them a native run has an opinion about.
 */

/*
 * The staging buffer a frame's views arrive through, and its layout.
 *
 * The glue has an XRViewerPose and needs to hand C a variable number of
 * matrices per frame, at 60-120 Hz, without allocating: it writes them
 * straight into this module's own static storage through the emscripten heap
 * views, and then calls xr_frame_publish() to say how many are there. One
 * call per frame, no marshalling, no allocation, and nothing crosses the
 * boundary that is not a number.
 *
 * A slot is four bytes either way, so one array serves both the matrices
 * (floats) and the eye/rect (int32) — the glue picks HEAPF32 or HEAP32 per
 * field. The union is what makes reading them back type-punned legally in C
 * rather than by aliasing a float array through an int pointer.
 *
 * THESE OFFSETS ARE MIRRORED IN JS. A C macro cannot be used inside an EM_JS
 * body (the body is stringified by the preprocessor, not expanded), so
 * xr_session.c writes the same numbers as literals with a pointer back here,
 * and xr_test.c asserts the two sets agree. Changing one means changing all
 * three.
 */
#define XR_STAGE_VIEW   0    /* 16 floats, column-major world-to-view */
#define XR_STAGE_PROJ   16   /* 16 floats, column-major projection */
#define XR_STAGE_POS    32   /* 3 floats, world-space eye position */
#define XR_STAGE_EYE    35   /* int32, enum xr_eye */
#define XR_STAGE_X      36   /* int32, viewport rect, framebuffer pixels */
#define XR_STAGE_Y      37
#define XR_STAGE_WIDTH  38
#define XR_STAGE_HEIGHT 39
#define XR_STAGE_STRIDE 40   /* slots per view */

union xr_stage_slot {
	float   f;
	int32_t i;
};

/* The staging buffer's base: XR_MAX_VIEWS * XR_STAGE_STRIDE slots. */
union xr_stage_slot *xr_stage(void);

/*
 * Record the probe's answer and log the one line the page gets out of this
 * module when there is no session to be had. Reported by number from JS, so
 * the enum's values are a contract (xr.h).
 */
void xr_report_support(int32_t support);

/*
 * The session is live: the layer is attached, the reference space is
 * acquired, and the glue is about to start pumping XR frames. Takes the frame
 * loop away from the browser's rAF (#991) — from here until the session ends,
 * every engine_tick comes from an XR animation frame, because that callback
 * is the only place an XRFrame, and therefore a pose, exists.
 *
 * Idempotent: a second call while a session is already running changes
 * nothing, so the loop can never be suspended twice and resumed once.
 */
void xr_session_begun(void);

/*
 * The session is over, for any reason. reason is XR_END_* below, and it is
 * only ever a log line — the teardown is identical either way, which is the
 * point: a session the user ended from the headset's system menu arrives
 * here through exactly the path a page-initiated exit does, because both are
 * the session's own "end" event.
 *
 * Idempotent, and safe to call when no session ever started: a request that
 * was rejected before a session existed reports its failure here, and finds
 * nothing to tear down.
 */
void xr_session_ended(int32_t reason);

#define XR_END_NORMAL   0   /* the session ended: button, or system menu */
#define XR_END_REFUSED  1   /* requestSession was rejected or unavailable */
#define XR_END_SETUP    2   /* a session started but could not be set up */

/*
 * Publish the staged views as the frame's list and point the WebGL backend's
 * backbuffer at the layer's framebuffer (#990).
 *
 * fbo is XRWebGLLayer.framebuffer as a GL name (see xr_session.c for how an
 * opaque framebuffer gets one), and fb_width/fb_height are the layer's full
 * framebuffer size. count is how many views the glue staged; it is clamped to
 * XR_MAX_VIEWS rather than trusted, because it is the one number here that
 * comes from a runtime rather than from this module.
 *
 * A zero fb_width or fb_height means the frame had no layer to report — a
 * session whose render state has not landed, or whose layer was replaced
 * between frames — and publishes an empty list with nothing declared, rather
 * than a rect with no area on a framebuffer that is not there.
 *
 * The declaration made here covers the WHOLE layer framebuffer, which is where
 * a frame starts and ends rather than where its eyes are drawn: each eye
 * narrows it to that eye's own rect as the renderer reaches it, and the end of
 * the view list puts it back (see xr_view_target in xr_state.c, and the
 * per-view callback in scene_renderer/scene_view.h). Anything in the frame that
 * is not one of the views — the UI compositing over the scene, today — thus
 * lands across the whole layer rather than inside whichever eye happened to be
 * drawn last.
 */
void xr_frame_publish(uint32_t fbo, uint32_t fb_width, uint32_t fb_height,
		      int32_t count);

/*
 * The depth range a session should be created with: the scene's own near and
 * far, or WebXR's defaults when there is no renderer to ask (#994).
 *
 * This runs the other way to everything else in this header — the glue calls
 * OUT to it rather than in — and it is here anyway because the policy is the
 * testable part and the updateRenderState call is not. WebXR owns depthNear /
 * depthFar and BUILDS each view's projection from them, so they are the one
 * number the engine and the runtime have to agree on: left at the runtime's
 * default, a scene framed to 100 metres would be drawn against a 1000-metre
 * frustum, spending its depth precision on ten times the range it uses.
 *
 * Never returns a range a session would reject: 0 < near < far, whatever the
 * renderer says.
 */
void xr_depth_range(float *near, float *far);

#endif /* XR_BRIDGE_H */
