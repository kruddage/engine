/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "xr_bridge.h"

#include <xr/xr.h>
#include <log/log.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include <stdint.h>

/*
 * The browser half of the session: the WebXR calls themselves, and the frame
 * callback that drives the engine while one is running. Everything here is
 * either an EM_JS body or the entry point that calls one, so none of it can
 * be exercised outside a browser — which is exactly why the state machine and
 * the view-list bookkeeping live next door in xr_state.c, where a native run
 * can hold them to account (xr_bridge.h says why the seam is where it is).
 *
 * The shape of the glue is one installed object rather than a handful of
 * loose EM_JS functions, because a session is not one call: it is a promise
 * chain that has to keep the session, the reference space and the frame
 * callback alive between calls, and EM_JS bodies share no scope of their own.
 * window.kruddXrGlue is that scope. It is the module's own working state and
 * not part of the page's surface — the page's surface is the exported
 * krudd_xr_* entry points at the bottom of this file, which is what an Enter
 * VR control calls (#997).
 */

#ifdef __EMSCRIPTEN__

/*
 * Install the glue. Idempotent, and called from every entry point rather than
 * from a boot hook, because nothing in the engine boots this module: no
 * subsystem registers it and no plugin table names it. The engine does not
 * know a session exists (#987) — the page does, and the page reaches this
 * module through the exports below, so the first of those calls is what
 * installs the glue.
 *
 * stage/stride/max_views describe the buffer a frame's views are written
 * into, and in_stage/in_stride/max_inputs the second buffer a frame's input
 * sources are written into (#996); see xr_bridge.h for both layouts, whose
 * field offsets are repeated as literals inside onFrame because a C macro
 * cannot be expanded inside an EM_JS body.
 */
EM_JS(void, xr_js_install, (void *stage, int32_t stride, int32_t max_views,
			    void *in_stage, int32_t in_stride,
			    int32_t max_inputs), {
	if (window.kruddXrGlue)
		return;

	var st = {
		session: null,
		space:   null,
		failed:  0,
		base:    stage >> 2,
		stride:  stride,
		max:     max_views,
		inBase:   in_stage >> 2,
		inStride: in_stride,
		inMax:    max_inputs,
		/*
		 * Presses that have landed and not yet been reported, one
		 * entry per XRInputSource: { src, n }. An ARRAY keyed by the
		 * source OBJECT rather than by index, because an index is not
		 * an identity — session.inputSources is rebuilt whenever a
		 * controller connects or disconnects, and a left hand that was
		 * slot 1 becomes slot 0 the moment the right one is put down.
		 * Attributing a press to whoever is standing in that slot next
		 * frame would fire a click from the wrong hand.
		 */
		sel: [],
		/*
		 * The depth range the next session is created with, filled in
		 * by request() from the engine's own near/far (#994). Held on
		 * the glue rather than passed through setup()'s promise chain
		 * because it is a property of the session being requested, and
		 * the chain already carries the two things that are not.
		 */
		near:    0.1,
		far:     1000.0
	};

	function note(line) {
		if (typeof console !== 'undefined')
			console.log('[krudd] xr: ' + line);
	}

	/*
	 * The WebGL context the engine's backend created and draws through
	 * (renderer_webgl.c). GLctx is emscripten's own live-context variable
	 * and Module.ctx is the same object under the name the runtime
	 * publishes it as; either one is the context, and asking for both
	 * costs nothing against a build that stops setting one of them.
	 */
	function context() {
		return (typeof GLctx !== 'undefined' && GLctx) ||
		       Module['ctx'] || null;
	}

	/*
	 * A GL name for XRWebGLLayer.framebuffer. That framebuffer is an
	 * OPAQUE WebGLFramebuffer: the browser hands out the object, but
	 * emscripten's GL tables have never seen it, so the C side has no
	 * integer to bind. Registering it in GL.framebuffers mints one, and
	 * glBindFramebuffer(name) then finds the very object WebXR gave us.
	 * The name is cached on the object itself, the same field emscripten
	 * stamps on framebuffers it creates, so a layer is registered once
	 * rather than every frame — and a replacement layer, being a
	 * different object, gets its own.
	 *
	 * Opaque is also why nothing may introspect it: it is only valid
	 * inside an XR animation frame, and glCheckFramebufferStatus against
	 * it throws. The backend was taught never to do either to a declared
	 * backbuffer (#990), which is what makes this safe to hand over.
	 *
	 * A null framebuffer is legal — a session on a device that composites
	 * from the default framebuffer reports one — and 0 is exactly what
	 * the backend reads as "the canvas's own", so it passes straight
	 * through.
	 */
	function fbName(fb) {
		if (!fb)
			return 0;
		if (!fb.name) {
			fb.name = GL.getNewId(GL.framebuffers);
			GL.framebuffers[fb.name] = fb;
		}
		return fb.name;
	}

	/*
	 * This source's pending-press tally, created on first sight. Linear
	 * search over at most a handful of entries, once per press and once
	 * per source per frame — a Map would key the same way and cost an
	 * allocation per session for nothing.
	 */
	function selOf(src) {
		for (var i = 0; i < st.sel.length; i++) {
			if (st.sel[i].src === src)
				return st.sel[i];
		}
		st.sel.push({ src: src, n: 0 });
		return st.sel[st.sel.length - 1];
	}

	/*
	 * Drop tallies for sources the session no longer lists. A controller
	 * that is switched off mid-session leaves whatever it had pending, and
	 * without this the array would grow for the life of the session and a
	 * press could be delivered to a hand that had already gone.
	 */
	function pruneSel(sources) {
		var keep = [];
		var i, j;

		for (i = 0; i < st.sel.length; i++) {
			for (j = 0; j < sources.length; j++) {
				if (sources[j] === st.sel[i].src) {
					keep.push(st.sel[i]);
					break;
				}
			}
		}
		st.sel = keep;
	}

	/*
	 * This frame's input sources, staged for xr_input_publish (#996).
	 *
	 * targetRaySpace, not gripSpace: WebXR defines the target ray as the
	 * aim a pointing UI is meant to use, and the grip as where the hand
	 * holding the thing is. Picking with the grip would aim from the fist.
	 *
	 * A source with no pose this frame is skipped rather than staged at
	 * the origin — an untracked controller is not a controller pointing at
	 * the floor — and the count returned is therefore how many sources
	 * actually had one, which is the number that reaches the engine.
	 *
	 * The press tally is taken and ZEROED here. That is what makes the
	 * click a one-frame edge: whatever landed between the last frame and
	 * this one is reported on this frame and on no other.
	 */
	function stageInputs(frame) {
		var sources = st.session.inputSources || [];
		var f32 = HEAPF32;
		var i32 = HEAP32;
		var n = 0;
		var i;

		for (i = 0; i < sources.length && n < st.inMax; i++) {
			var src = sources[i];
			var pose = (st.space && src.targetRaySpace)
				? frame.getPose(src.targetRaySpace, st.space)
				: null;

			if (!pose)
				continue;

			var m = pose.transform.matrix;
			var e = selOf(src);
			var o = st.inBase + n * st.inStride;

			/* Offsets: xr_bridge.h's XR_IN_*. */
			f32[o + 0] = pose.transform.position.x;
			f32[o + 1] = pose.transform.position.y;
			f32[o + 2] = pose.transform.position.z;
			/*
			 * Forward is the ray space's -Z, which is the third
			 * column of its pose matrix negated. Read off the
			 * matrix rather than rotated out of the orientation
			 * quaternion by hand: the matrix is the same rotation
			 * already expanded, and WebXR hands it over for free.
			 */
			f32[o + 3] = -m[8];
			f32[o + 4] = -m[9];
			f32[o + 5] = -m[10];
			i32[o + 6] = src.handedness === 'left' ? 1 :
				     src.handedness === 'right' ? 2 : 0;
			i32[o + 7] = e.n;
			e.n = 0;
			n++;
		}
		pruneSel(sources);
		return n;
	}

	/*
	 * One XR frame. This is the engine's frame loop for as long as a
	 * session runs: stage the views, publish them, and run exactly one
	 * tick (#991's krudd_driven_tick, the same engine_tick the browser's
	 * rAF calls when the flat page is driving).
	 *
	 * The re-arm comes first so a throw further down cannot silently stop
	 * the loop and leave the headset on a frozen frame with the flat
	 * page's rAF still suspended.
	 *
	 * A frame with no viewer pose — tracking lost, or the headset off the
	 * user's head — publishes zero views and STILL ticks. The engine
	 * keeps running (audio, scripts, timers); it just has nothing to draw
	 * for the headset this frame.
	 */
	function onFrame(time, frame) {
		var s = st.session;

		if (!s)
			return;
		s.requestAnimationFrame(onFrame);

		var layer = s.renderState.baseLayer;
		var pose  = st.space ? frame.getViewerPose(st.space) : null;
		var n = 0;

		if (pose && layer) {
			/*
			 * Read the heap views fresh every frame: growing the
			 * heap replaces them, and the stage pointer is the
			 * only part of this that is stable across a growth.
			 */
			var f32 = HEAPF32;
			var i32 = HEAP32;
			var views = pose.views;

			n = views.length < st.max ? views.length : st.max;
			for (var v = 0; v < n; v++) {
				var view = views[v];
				var vp = layer.getViewport(view);
				var m = view.transform.inverse.matrix;
				var p = view.projectionMatrix;
				var o = st.base + v * st.stride;

				/* Offsets: xr_bridge.h's XR_STAGE_*. */
				for (var i = 0; i < 16; i++) {
					f32[o + i] = m[i];
					f32[o + 16 + i] = p[i];
				}
				f32[o + 32] = view.transform.position.x;
				f32[o + 33] = view.transform.position.y;
				f32[o + 34] = view.transform.position.z;
				i32[o + 35] = view.eye === 'left' ? 1 :
					      view.eye === 'right' ? 2 : 0;
				i32[o + 36] = vp ? vp.x : 0;
				i32[o + 37] = vp ? vp.y : 0;
				i32[o + 38] = vp ? vp.width : 0;
				i32[o + 39] = vp ? vp.height : 0;
			}
		}

		/*
		 * The controllers ride along with the views, staged every
		 * frame — including the frames with none, which is how a
		 * controller that was put down stops pointing (#996). Staged
		 * outside the pose branch above deliberately: a frame that
		 * lost the VIEWER's pose has not necessarily lost the hands',
		 * and there is no reason to stop aiming because the head went
		 * missing for a frame.
		 */
		_krudd_xr_frame(fbName(layer && layer.framebuffer),
				layer ? layer.framebufferWidth : 0,
				layer ? layer.framebufferHeight : 0, n,
				stageInputs(frame));
		_krudd_driven_tick();
	}

	/*
	 * Everything that has to be true before the first XR frame, in the
	 * order WebXR requires it:
	 *
	 *   makeXRCompatible   the context must be running on the adapter the
	 *                      headset is on before it may back a layer. See
	 *                      the comment at the context-creation site in
	 *                      renderer_webgl.c for why this happens here,
	 *                      after the context exists, and what it costs.
	 *   XRWebGLLayer       the target the runtime composites, and the
	 *                      framebuffer every view's viewport is a rect in.
	 *   depthNear/depthFar the range the runtime BUILDS each view's
	 *                      projection from — the engine does not get to
	 *                      choose a frustum here, only to say what range it
	 *                      wants one over, so this is the one place the two
	 *                      sides' near/far are made to agree (#994). Set in
	 *                      the same updateRenderState as the layer: they
	 *                      take effect together, before any frame, rather
	 *                      than a frame apart with one frame drawn against
	 *                      a range nothing asked for.
	 *   reference space    what poses are reported relative to.
	 *
	 * local-floor puts the origin on the floor, which is the space the
	 * engine's metre convention is written against (math/camera.h). Not
	 * every runtime offers it; local is the same space with the origin at
	 * the head's starting height, so a runtime that refuses the first
	 * gets the second rather than getting no session.
	 */
	function setup(session, gl) {
		return gl.makeXRCompatible().then(function () {
			var layer = new XRWebGLLayer(session, gl);

			session.updateRenderState({
				baseLayer: layer,
				depthNear: st.near,
				depthFar:  st.far
			});
			return session.requestReferenceSpace('local-floor')
				.catch(function () {
					note('no local-floor; using local');
					return session.requestReferenceSpace(
						'local');
				});
		}).then(function (space) {
			st.space = space;
			_krudd_xr_session_begun();
			session.requestAnimationFrame(onFrame);
		});
	}

	window.kruddXrGlue = {
		/*
		 * The numbers are enum xr_support (xr.h), reported by value:
		 * 0 YES, 1 NO_API, 2 NO_DEVICE, 3 PROBE_ERROR. A missing
		 * navigator.xr is answered without a promise because there is
		 * nothing to ask; the rejection arm is a page whose
		 * permissions policy forbids xr-spatial-tracking, which
		 * throws rather than answering false.
		 */
		probe: function () {
			if (!navigator.xr) {
				_krudd_xr_report_support(1);
				return;
			}
			try {
				navigator.xr.isSessionSupported('immersive-vr')
					.then(function (ok) {
						_krudd_xr_report_support(
							ok ? 0 : 2);
					}, function () {
						_krudd_xr_report_support(3);
					});
			} catch (e) {
				_krudd_xr_report_support(3);
			}
		},

		/*
		 * Must be called from a user gesture — WebXR refuses an
		 * immersive session requested from anywhere else, which is a
		 * rejection this reports like any other.
		 *
		 * Every failure path ends at _krudd_xr_session_ended, and the
		 * ones that already have a session end that session rather
		 * than dropping it: ending it fires the "end" event, and that
		 * event is the single teardown path (see the listener below).
		 */
		request: function (depthNear, depthFar) {
			var gl = context();

			if (st.session)
				return;
			if (!navigator.xr || !gl) {
				_krudd_xr_session_ended(1);
				return;
			}
			st.failed = 0;
			st.near = depthNear;
			st.far = depthFar;
			st.sel = [];
			navigator.xr.requestSession('immersive-vr')
				.then(function (session) {
					st.session = session;
					/*
					 * The press, counted where it lands
					 * (#996). `select` is the WebXR event
					 * for a completed primary action and
					 * fires once per press, on the source
					 * that made it — so a tally per source
					 * is all a one-frame click edge needs,
					 * and the frame callback spends it.
					 *
					 * Listened for rather than polled off
					 * gamepad.buttons[0].pressed: a poll
					 * at frame rate misses a press that
					 * begins and ends between two frames,
					 * and `select` is the mapping-neutral
					 * name for "the trigger", which a
					 * button index is not.
					 */
					session.addEventListener('select',
						function (ev) {
							selOf(ev.inputSource)
								.n++;
						});
					session.addEventListener('end',
						function () {
							st.session = null;
							st.space = null;
							st.sel = [];
							_krudd_xr_session_ended(
								st.failed
								? 2 : 0);
						});
					return setup(session, gl);
				}).catch(function (e) {
					var s = st.session;

					note('session failed: ' + e);
					if (!s) {
						_krudd_xr_session_ended(1);
						return;
					}
					st.failed = 1;
					try {
						s.end();
					} catch (e2) {
						st.session = null;
						st.space = null;
						_krudd_xr_session_ended(2);
					}
				});
		},

		/*
		 * Ask the browser to end the session and nothing else. The
		 * unwinding happens in the "end" listener, which is also
		 * where a session the user ended from the headset's system
		 * menu arrives — so the page's exit and the user's are the
		 * same code path rather than two that have to agree.
		 */
		end: function () {
			if (!st.session)
				return;
			try {
				st.session.end();
			} catch (e) {
				note('end failed: ' + e);
			}
		}
	};
})

EM_JS(void, xr_js_probe, (void), {
	window.kruddXrGlue.probe();
})

EM_JS(void, xr_js_request_session, (float depth_near, float depth_far), {
	window.kruddXrGlue.request(depth_near, depth_far);
})

EM_JS(void, xr_js_end_session, (void), {
	window.kruddXrGlue.end();
})

/*
 * Whether the page booted the WebGPU backend. XR is a WebGL-only path in this
 * initiative and says so in the code (#987): XRGPUBinding is not broadly
 * shipped, so a session on a WebGPU page would take the frame loop, declare a
 * backbuffer the live backend does not read, and put the user in a headset
 * showing nothing. Refusing is the honest answer; routing them to the GL path
 * is the page's job (#997).
 *
 * The page owns the decision — window.kruddWantsWebGPU is the single source of
 * truth engine.c's backend selection reads too, so this cannot disagree with
 * what actually booted. A page that does not answer is not a page running
 * WebGPU: the absence of the hook is not evidence, so it does not refuse.
 */
EM_JS(int32_t, xr_js_wants_webgpu, (void), {
	try {
		return window.kruddWantsWebGPU() ? 1 : 0;
	} catch (e) {
		return 0;
	}
})

static void xr_install(void)
{
	xr_js_install(xr_stage(), XR_STAGE_STRIDE, XR_MAX_VIEWS,
		      xr_input_stage(), XR_IN_STRIDE, XR_MAX_INPUT_SOURCES);
}
#endif /* __EMSCRIPTEN__ */

void xr_probe(void)
{
	if (xr_supported() != XR_SUPPORT_UNKNOWN)
		return;
#ifdef __EMSCRIPTEN__
	xr_install();
	xr_js_probe();
#else
	/*
	 * There is no navigator outside the browser, so a native build gets
	 * the same answer — and the same one line — a desktop browser with no
	 * WebXR gets, through the same door rather than through a second one
	 * that could drift from it. This is the case the module spends most
	 * of its life in, and it is the case a native test can actually run.
	 */
	xr_report_support(XR_SUPPORT_NO_API);
#endif
}

void xr_request_session(void)
{
	enum xr_support s = xr_supported();
#ifdef __EMSCRIPTEN__
	float near, far;
#endif

	if (xr_session_active())
		return;
	/*
	 * A definite no is refused here rather than handed to the browser to
	 * reject: the answer is already known, and a request that cannot
	 * succeed should not cost the page a rejected promise. UNKNOWN is let
	 * through — the probe may simply not have answered yet, and the
	 * browser is the authority in that case, not this cache.
	 */
	if (s != XR_SUPPORT_YES && s != XR_SUPPORT_UNKNOWN) {
		LOG_WARN("xr: immersive-vr is not available on this page; "
			 "the session request is ignored");
		return;
	}
#ifdef __EMSCRIPTEN__
	if (xr_js_wants_webgpu()) {
		LOG_WARN("xr: this page booted the WebGPU backend and WebXR "
			 "here is WebGL-only; reload with ?renderer=webgl");
		return;
	}
	/*
	 * The engine's depth range, read at request time and applied with the
	 * layer (#994). Read here rather than per frame because depthNear /
	 * depthFar are session render state, not frame state: the runtime builds
	 * every view's projection from them, and changing them mid-session
	 * changes the frustum under a viewer's eyes. The renderer's range is a
	 * constant of the authored camera, so once is exactly as often as it
	 * needs asking.
	 */
	xr_depth_range(&near, &far);
	xr_install();
	xr_js_request_session(near, far);
#else
	LOG_WARN("xr: no browser to request a session from; ignored");
#endif
}

void xr_end_session(void)
{
#ifdef __EMSCRIPTEN__
	/*
	 * Deliberately not guarded on xr_session_active(). The glue holds the
	 * session handle and is the authority on whether there is one, and
	 * there is a window — after requestSession() resolves, before the
	 * layer and the reference space are up — where a session exists that
	 * this side has not been told about yet. An exit pressed in that
	 * window has to reach it, and the glue is a no-op when there really is
	 * nothing to end.
	 */
	xr_install();
	xr_js_end_session();
#endif
}

#ifdef __EMSCRIPTEN__
/*
 * The module's JS-facing surface, and the reason these are the only entry
 * points in this file with a krudd_ prefix: they are what crosses out of the
 * WASM module, so they are listed in EXPORTED_FUNCTIONS (kruddmake/ninja.scm)
 * the way _krudd_load_project and the #991 loop exports are, and named the
 * way the page names things.
 *
 * They split into two directions. probe/request/end/supported/active are what
 * the PAGE calls in — an Enter VR control (#997), or a console, which is how
 * this PR is exercised while no control exists yet. report_support/
 * session_begun/session_ended/frame are what the GLUE ABOVE calls back, and
 * are no part of what a page should ever call: they do not request anything,
 * they report that something already happened.
 *
 * Thin on purpose. Everything they do is one call into the module's own C, so
 * that the browser boundary is a boundary and not a place logic hides. The one
 * that is two calls is krudd_xr_frame, and the note on it says why.
 */
EMSCRIPTEN_KEEPALIVE void krudd_xr_probe(void)
{
	xr_probe();
}

EMSCRIPTEN_KEEPALIVE int32_t krudd_xr_supported(void)
{
	return (int32_t)xr_supported();
}

EMSCRIPTEN_KEEPALIVE void krudd_xr_request_session(void)
{
	xr_request_session();
}

EMSCRIPTEN_KEEPALIVE void krudd_xr_end_session(void)
{
	xr_end_session();
}

EMSCRIPTEN_KEEPALIVE int32_t krudd_xr_session_active(void)
{
	return xr_session_active();
}

EMSCRIPTEN_KEEPALIVE void krudd_xr_report_support(int32_t support)
{
	xr_report_support(support);
}

EMSCRIPTEN_KEEPALIVE void krudd_xr_session_begun(void)
{
	xr_session_begun();
}

EMSCRIPTEN_KEEPALIVE void krudd_xr_session_ended(int32_t reason)
{
	xr_session_ended(reason);
}

/*
 * The one exception to "one call into the module's own C": a frame carries the
 * views AND the input sources, and they arrive through this single export
 * rather than through one each (#996).
 *
 * Not for tidiness — because the module's exported names are a list the page's
 * build repeats (EXPORTED_FUNCTIONS in kruddmake/ninja.scm, and
 * ENGINE_EXPORTED_FUNCTIONS beside it), and a second name would have to be
 * added in both places to buy nothing a fifth argument does not. The C behind
 * it stays two functions, which is what the native test drives.
 *
 * Input first, so the pointers are published before the views they were aimed
 * with land — and unconditionally, because a frame that cannot report a view
 * (no layer yet) can still report a hand.
 */
EMSCRIPTEN_KEEPALIVE void krudd_xr_frame(uint32_t fbo, uint32_t fb_width,
					 uint32_t fb_height, int32_t count,
					 int32_t inputs)
{
	xr_input_publish(inputs);
	xr_frame_publish(fbo, fb_width, fb_height, count);
}
#endif /* __EMSCRIPTEN__ */
