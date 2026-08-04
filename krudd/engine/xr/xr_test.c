/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "xr_bridge.h"

#include <xr/xr.h>
#include <log/log.h>
#include <webgl/renderer_webgl.h>

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

/*
 * Native coverage for the half of the XR module that is not the browser: the
 * session state machine and the per-frame view list. There is no navigator
 * here, no session and no GL context, which is precisely the state the module
 * spends most of its life in — a desktop page with no headset — so the "no XR
 * available" path is not simulated below, it is the real thing.
 *
 * What this cannot reach is xr_session.c's glue: the promise chain, the XR
 * animation frame, the layer. Those need a browser and a headset, and the
 * split between the two translation units is what keeps the untestable half
 * small. Everything the glue would do to this module, it does through
 * xr_bridge.h — which is what these tests drive in its place.
 *
 * The module's state is global (there is exactly one session per page), so
 * the order below is deliberate: the probe tests run before anything can have
 * answered it, and every session test ends the session it started.
 */

static int tests_run;
static int tests_passed;

#define RUN(name) do { \
	tests_run++; \
	test_##name(); \
	tests_passed++; \
	printf("PASS: " #name "\n"); \
} while (0)

/*
 * A recognisable view in the staging buffer, exactly as the glue writes one:
 * matrix elements keyed to the view index so a mix-up between the two eyes,
 * or between view and projection, cannot pass.
 */
static void stage_view(int32_t index, float key, int32_t eye,
		       int32_t x, int32_t y, int32_t w, int32_t h)
{
	union xr_stage_slot *s = xr_stage() + index * XR_STAGE_STRIDE;
	int32_t i;

	for (i = 0; i < 16; i++) {
		s[XR_STAGE_VIEW + i].f = key + (float)i;
		s[XR_STAGE_PROJ + i].f = key + 100.0f + (float)i;
	}
	s[XR_STAGE_POS + 0].f = key + 200.0f;
	s[XR_STAGE_POS + 1].f = key + 201.0f;
	s[XR_STAGE_POS + 2].f = key + 202.0f;

	s[XR_STAGE_EYE].i    = eye;
	s[XR_STAGE_X].i      = x;
	s[XR_STAGE_Y].i      = y;
	s[XR_STAGE_WIDTH].i  = w;
	s[XR_STAGE_HEIGHT].i = h;
}

static void assert_view(const struct xr_view *v, float key, int32_t eye,
			int32_t x, int32_t y, int32_t w, int32_t h)
{
	int32_t i;

	for (i = 0; i < 16; i++) {
		assert(v->view.m[i] == key + (float)i);
		assert(v->proj.m[i] == key + 100.0f + (float)i);
	}
	assert(v->position[0] == key + 200.0f);
	assert(v->position[1] == key + 201.0f);
	assert(v->position[2] == key + 202.0f);

	assert(v->eye == eye);
	assert(v->x == x);
	assert(v->y == y);
	assert(v->width == w);
	assert(v->height == h);
}

/* Nothing declared, no views, no session: the state a flat page runs in. */
static void assert_flat_page(void)
{
	struct webgl_backbuffer_decl decl = webgl_backbuffer_declared();

	assert(xr_session_active() == 0);
	assert(xr_views() != NULL);
	assert(xr_views()->count == 0);
	assert(decl.declared == 0);
}

/*
 * Before anything has been asked of this module it must already be answering
 * as a page with no session: a valid empty list rather than a NULL a consumer
 * would have to guard, and a backbuffer nobody has redirected.
 */
static void test_views_are_a_valid_empty_list_at_rest(void)
{
	assert_flat_page();
	assert(xr_supported() == XR_SUPPORT_UNKNOWN);
}

/*
 * The common case, and the whole of what this module does to a page it cannot
 * run on: answer the question and log one line. No session, no view list, no
 * backbuffer declaration, nothing touched.
 */
static void test_probe_reports_no_api_without_a_browser(void)
{
	xr_probe();
	assert(xr_supported() == XR_SUPPORT_NO_API);
	assert_flat_page();
}

/*
 * The answer lands once. A second probe (a page that asks again, or two
 * callers each making sure) must not re-log the line or overwrite the answer
 * — one clear line is the contract, not one per caller.
 */
static void test_the_probe_answers_once(void)
{
	xr_probe();
	xr_report_support(XR_SUPPORT_YES);
	assert(xr_supported() == XR_SUPPORT_NO_API);
	assert_flat_page();
}

/*
 * A request made where a session cannot happen. The page must come out of it
 * exactly as it went in — which is the acceptance criterion this module is
 * most likely to be judged on, because it is what every desktop visitor gets.
 */
static void test_request_is_refused_and_changes_nothing(void)
{
	xr_request_session();
	assert_flat_page();

	/* And ending one that never began is the same non-event. */
	xr_end_session();
	assert_flat_page();
}

/*
 * The staging layout is written twice — once as the XR_STAGE_* macros the C
 * unpacks with, once as literals inside xr_session.c's EM_JS body, because a
 * macro cannot be expanded there. Nothing but this pins the two together, so
 * the literals are repeated here: a change to the layout that misses the JS
 * fails a build rather than shipping a headset full of garbage matrices.
 */
static void test_stage_layout_matches_the_glue(void)
{
	assert(XR_STAGE_VIEW == 0);
	assert(XR_STAGE_PROJ == 16);
	assert(XR_STAGE_POS == 32);
	assert(XR_STAGE_EYE == 35);
	assert(XR_STAGE_X == 36);
	assert(XR_STAGE_Y == 37);
	assert(XR_STAGE_WIDTH == 38);
	assert(XR_STAGE_HEIGHT == 39);
	assert(XR_STAGE_STRIDE == 40);
	assert(XR_MAX_VIEWS == 4);
}

/*
 * Entering twice is one entry. The glue only reports a session once, but the
 * flag is what pairs the loop's suspend with its resume (#991) — so a second
 * begin must not be able to open a second suspend that only one end would
 * close.
 */
static void test_session_begin_is_idempotent(void)
{
	xr_session_begun();
	assert(xr_session_active() == 1);
	xr_session_begun();
	assert(xr_session_active() == 1);

	xr_session_ended(XR_END_NORMAL);
	assert_flat_page();
}

/*
 * A stereo frame: two views, one framebuffer, two rects side by side in it —
 * the shape every headset produces. Each entry comes back exactly as it was
 * staged, because "verbatim" is the whole point of the pair (#988): an
 * off-axis projection that is adjusted on the way through is a projection
 * that no longer matches the lens it was measured for.
 */
static void test_publish_unpacks_each_view_verbatim(void)
{
	const struct xr_view_list *list;

	xr_session_begun();
	stage_view(0, 1.0f, XR_EYE_LEFT, 0, 0, 960, 1080);
	stage_view(1, 2.0f, XR_EYE_RIGHT, 960, 0, 960, 1080);
	xr_frame_publish(7, 1920, 1080, 2);

	list = xr_views();
	assert(list->fbo == 7);
	assert(list->count == 2);
	assert_view(&list->views[0], 1.0f, XR_EYE_LEFT, 0, 0, 960, 1080);
	assert_view(&list->views[1], 2.0f, XR_EYE_RIGHT, 960, 0, 960, 1080);

	xr_session_ended(XR_END_NORMAL);
}

/*
 * Publishing points the WebGL backend at the layer's framebuffer (#990), over
 * the whole of it — the per-eye rects live in the view list until #994 draws
 * them one at a time.
 */
static void test_publish_declares_the_layer_framebuffer(void)
{
	struct webgl_backbuffer_decl decl;

	xr_session_begun();
	stage_view(0, 1.0f, XR_EYE_LEFT, 0, 0, 960, 1080);
	xr_frame_publish(7, 1920, 1080, 1);

	decl = webgl_backbuffer_declared();
	assert(decl.declared == 1);
	assert(decl.fbo == 7);
	assert(decl.x == 0);
	assert(decl.y == 0);
	assert(decl.width == 1920);
	assert(decl.height == 1080);

	xr_session_ended(XR_END_NORMAL);
}

/*
 * The view count is the runtime's number, not this module's, so it is clamped
 * rather than trusted: a quad-view runtime reporting more views than the list
 * holds gets the ones that fit, and a runtime reporting nonsense gets none.
 */
static void test_publish_clamps_the_view_count(void)
{
	xr_session_begun();
	xr_frame_publish(7, 1920, 1080, XR_MAX_VIEWS + 3);
	assert(xr_views()->count == XR_MAX_VIEWS);

	xr_frame_publish(7, 1920, 1080, -1);
	assert(xr_views()->count == 0);

	xr_session_ended(XR_END_NORMAL);
}

/*
 * A frame with no viewer pose — tracking lost, or the headset off the user's
 * head — publishes no views and is not an error. The session is still
 * running and the engine still ticks; there is simply nothing to draw for the
 * headset, and last frame's views must not be left lying around to be drawn
 * again from a pose that no longer holds.
 */
static void test_a_poseless_frame_publishes_no_views(void)
{
	xr_session_begun();
	stage_view(0, 1.0f, XR_EYE_LEFT, 0, 0, 960, 1080);
	xr_frame_publish(7, 1920, 1080, 1);
	assert(xr_views()->count == 1);

	xr_frame_publish(7, 1920, 1080, 0);
	assert(xr_views()->count == 0);
	assert(xr_session_active() == 1);

	xr_session_ended(XR_END_NORMAL);
}

/*
 * A frame whose session has no layer to draw into yet. Nothing is declared —
 * a zero-sized rect on a framebuffer that is not there would swallow the
 * frame — and any declaration from a previous layer goes with it, so the
 * frame lands on the canvas rather than somewhere that no longer exists.
 */
static void test_a_layerless_frame_declares_nothing(void)
{
	xr_session_begun();
	stage_view(0, 1.0f, XR_EYE_LEFT, 0, 0, 960, 1080);
	xr_frame_publish(7, 1920, 1080, 1);
	assert(webgl_backbuffer_declared().declared == 1);

	xr_frame_publish(0, 0, 0, 1);
	assert(xr_views()->count == 0);
	assert(xr_views()->fbo == 0);
	assert(webgl_backbuffer_declared().declared == 0);
	assert(xr_session_active() == 1);

	xr_session_ended(XR_END_NORMAL);
	assert_flat_page();
}

/*
 * Leaving a session puts the page back exactly where it was: no views, no
 * session, and — the one that would break the flat page if it were missed —
 * no backbuffer declaration, because the framebuffer it named belonged to the
 * layer and stops existing with the session.
 */
static void test_session_end_restores_the_flat_page(void)
{
	xr_session_begun();
	stage_view(0, 1.0f, XR_EYE_LEFT, 0, 0, 960, 1080);
	xr_frame_publish(7, 1920, 1080, 1);

	xr_session_ended(XR_END_NORMAL);
	assert_flat_page();
	assert(xr_views()->fbo == 0);
}

/*
 * The user takes the headset off and ends the session from the system menu,
 * and the page's own Exit VR is pressed on the way: two ends, one teardown.
 * The page never controls the first of those, so the second must find nothing
 * left to undo rather than resuming a loop that is already running.
 */
static void test_session_end_is_idempotent(void)
{
	xr_session_begun();
	xr_frame_publish(7, 1920, 1080, 1);

	xr_session_ended(XR_END_NORMAL);
	xr_session_ended(XR_END_NORMAL);
	assert_flat_page();
}

/*
 * A request that was refused before any session existed reports its failure
 * through the same door a real end comes through, and finds nothing to tear
 * down. Nothing about the page changes on the way past.
 */
static void test_a_failure_without_a_session_changes_nothing(void)
{
	xr_session_ended(XR_END_REFUSED);
	assert_flat_page();

	xr_session_ended(XR_END_SETUP);
	assert_flat_page();
}

int main(void)
{
	log_init();

	RUN(views_are_a_valid_empty_list_at_rest);
	RUN(probe_reports_no_api_without_a_browser);
	RUN(the_probe_answers_once);
	RUN(request_is_refused_and_changes_nothing);
	RUN(stage_layout_matches_the_glue);
	RUN(session_begin_is_idempotent);
	RUN(publish_unpacks_each_view_verbatim);
	RUN(publish_declares_the_layer_framebuffer);
	RUN(publish_clamps_the_view_count);
	RUN(a_poseless_frame_publishes_no_views);
	RUN(a_layerless_frame_declares_nothing);
	RUN(session_end_restores_the_flat_page);
	RUN(session_end_is_idempotent);
	RUN(a_failure_without_a_session_changes_nothing);

	printf("%d/%d tests passed\n", tests_passed, tests_run);
	return tests_passed == tests_run ? 0 : 1;
}
