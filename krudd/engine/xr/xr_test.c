/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "xr_bridge.h"

#include <xr/xr.h>
#include <log/log.h>
#include <math/camera.h>
#include <math/math_types.h>
#include <scene_renderer/scene_view.h>
#include <viewport/pointer3d.h>
#include <webgl/renderer_webgl.h>

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

/*
 * The scene renderer, as far as this module can tell (#994).
 *
 * These four are the whole of what xr reaches for in render/scene_renderer, and
 * natively they are defined right here rather than linked: scene_renderer is a
 * WASM-only library (it is a plugin of the browser build and needs a GPU), so
 * there is no archive to link, and in the browser both modules are objects in
 * the one WASM image and the definitions are the real ones. See xr/build.scm.
 *
 * Which turns the link problem into the test's best instrument. What this
 * module actually DOES with a headset's frame is hand a list of views to a
 * renderer and answer, per view, where that view lands — so standing in for the
 * renderer is standing exactly where the frame can be read back and checked,
 * without a browser, a headset or a GL context anywhere near it.
 */
static struct scene_view    g_drawn[SCENE_MAX_VIEWS];
static uint32_t             g_drawn_count;
static scene_view_target_fn g_target_fn;
static void                *g_target_user;
static struct camera        g_scene_cam;
static int32_t              g_scene_cam_ready;

void scene_renderer_set_views(const struct scene_view *views, uint32_t count)
{
	uint32_t i;

	assert(views && count > 0 && count <= SCENE_MAX_VIEWS);
	for (i = 0; i < count; i++)
		g_drawn[i] = views[i];
	g_drawn_count = count;
}

void scene_renderer_clear_views(void)
{
	g_drawn_count = 0;
}

void scene_renderer_set_view_target(scene_view_target_fn fn, void *user)
{
	g_target_fn   = fn;
	g_target_user = user;
}

int32_t scene_renderer_scene_camera(struct camera *out)
{
	if (!out || !g_scene_cam_ready)
		return 0;
	*out = g_scene_cam;
	return 1;
}

/*
 * The world-space pointer, as far as this module can tell (#996).
 *
 * Same trick, same reason: ui/viewport is a wasm-only library with no native
 * archive, and in the browser both modules are objects in the one WASM image,
 * so the definitions here stand in for the real ones and the call the XR module
 * makes DOWNWARD every frame becomes the place a controller's ray can be read
 * back and checked — with no browser, no headset and no hand.
 *
 * A count of -1 means "cleared", which is a different observation from
 * "published zero pointers" and the tests below have to be able to tell them
 * apart.
 */
static struct pointer3d_ray g_pointers[POINTER3D_MAX];
static int32_t              g_pointer_count = -1;

void viewport_pointer3d_publish(const struct pointer3d_ray *rays, int32_t count)
{
	int32_t i;

	assert(count >= 0 && count <= POINTER3D_MAX);
	assert(rays || count == 0);
	for (i = 0; i < count; i++)
		g_pointers[i] = rays[i];
	g_pointer_count = count;
}

void viewport_pointer3d_clear(void)
{
	g_pointer_count = -1;
}

/* The scene's camera, as a scene that has initialised would report it. */
static void set_scene_camera(float x, float y, float z, float near, float far)
{
	float eye[3]    = { x, y, z };
	float target[3] = { x, y, z - 1.0f };
	float up[3]     = { 0.0f, 1.0f, 0.0f };
	int   i;

	memset(&g_scene_cam, 0, sizeof(g_scene_cam));
	for (i = 0; i < 3; i++) {
		g_scene_cam.eye[i]    = eye[i];
		g_scene_cam.target[i] = target[i];
		g_scene_cam.up[i]     = up[i];
	}
	g_scene_cam.fov_y  = 0.8f;
	g_scene_cam.aspect = 1.6f;
	g_scene_cam.near   = near;
	g_scene_cam.far    = far;
	camera_update(&g_scene_cam);
	g_scene_cam_ready = 1;
}

static void no_scene_camera(void)
{
	g_scene_cam_ready = 0;
}

static int close_enough(float a, float b)
{
	float d = a - b;

	return (d < 0.0f ? -d : d) < 1e-4f;
}

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

/*
 * A real head, staged the way the glue stages one: a rigid pose and the
 * projection that belongs to it, written as floats into the same slots
 * xr_session.c's EM_JS writes.
 *
 * The projection is deliberately OFF-AXIS — an asymmetric frustum is what a
 * headset actually reports, and it is the thing that cannot survive being
 * re-derived from a field of view, so every assertion about it below is an
 * assertion that nothing tried.
 */
static void stage_pose(int32_t index, const struct mat4 *view,
		       const struct mat4 *proj, const float pos[3],
		       int32_t eye, int32_t x, int32_t y, int32_t w, int32_t h)
{
	union xr_stage_slot *s = xr_stage() + index * XR_STAGE_STRIDE;
	int32_t i;

	for (i = 0; i < 16; i++) {
		s[XR_STAGE_VIEW + i].f = view->m[i];
		s[XR_STAGE_PROJ + i].f = proj->m[i];
	}
	for (i = 0; i < 3; i++)
		s[XR_STAGE_POS + i].f = pos[i];

	s[XR_STAGE_EYE].i    = eye;
	s[XR_STAGE_X].i      = x;
	s[XR_STAGE_Y].i      = y;
	s[XR_STAGE_WIDTH].i  = w;
	s[XR_STAGE_HEIGHT].i = h;
}

/* A head at POS looking at TARGET, and the world-to-view matrix that is. */
static void head_pose(struct mat4 *out, const float pos[3],
		      const float target[3])
{
	float up[3] = { 0.0f, 1.0f, 0.0f };

	mat4_look_at(out, pos, target, up);
}

/*
 * An off-axis frustum, GL convention (NDC z in [-1, 1]) — the convention WebXR
 * reports a projection in, and the one camera.h asks a supplied projection to
 * arrive in. Built by hand because mat4_perspective can only produce symmetric
 * ones, which is the whole reason a view is a supplied pair (#988).
 */
static void off_axis_proj(struct mat4 *out, float l, float r, float b, float t,
			  float n, float f)
{
	memset(out->m, 0, sizeof(out->m));
	out->m[0]  = 2.0f * n / (r - l);
	out->m[5]  = 2.0f * n / (t - b);
	out->m[8]  = (r + l) / (r - l);
	out->m[9]  = (t + b) / (t - b);
	out->m[10] = -(f + n) / (f - n);
	out->m[11] = -1.0f;
	out->m[14] = -2.0f * f * n / (f - n);
}

/* Nothing declared, no views, no session: the state a flat page runs in. */
static void assert_flat_page(void)
{
	struct webgl_backbuffer_decl decl = webgl_backbuffer_declared();

	assert(xr_session_active() == 0);
	assert(xr_views() != NULL);
	assert(xr_views()->count == 0);
	assert(decl.declared == 0);

	/*
	 * And the renderer is back on its own camera, with nobody left holding
	 * the per-view target callback. A page that came out of a session still
	 * drawing two eyes into a framebuffer that no longer exists is the
	 * failure this module is most able to cause and least able to notice.
	 */
	assert(g_drawn_count == 0);
	assert(g_target_fn == NULL);

	/*
	 * And nothing is still pointing. A ray left published after a session
	 * would hang in the flat page's scene, aimed from a hand that left the
	 * room with the headset (#996).
	 */
	assert(g_pointer_count == -1);
}

/*
 * An input source in the staging buffer, exactly as the glue writes one: the
 * targetRaySpace origin and its forward, the handedness, and how many presses
 * landed since the last frame.
 */
static void stage_input(int32_t index, const float origin[3],
			const float dir[3], int32_t hand, int32_t selects)
{
	union xr_stage_slot *s = xr_input_stage() + index * XR_IN_STRIDE;
	int32_t              i;

	for (i = 0; i < 3; i++) {
		s[XR_IN_ORIGIN + i].f = origin[i];
		s[XR_IN_DIR + i].f    = dir[i];
	}
	s[XR_IN_HAND].i   = hand;
	s[XR_IN_SELECT].i = selects;
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

/*
 * The stereo frame, arriving at the renderer (#994). Two views, two rects in
 * one framebuffer, and everything a headset measured reaching the list intact:
 * the rigid pose as a view matrix, the off-axis frustum as a projection, and
 * the eye position that goes with them.
 *
 * The stage is at the origin here so that "composed" and "verbatim" are the
 * same numbers — what the stage does to them is the next test's subject. The
 * pair is asserted through view_proj as well as through its halves, because
 * view_proj is what a pass actually uploads and a camera whose product does not
 * match its factors would draw a frame nothing in the list explains.
 */
static void test_a_stereo_frame_reaches_the_renderer(void)
{
	static const float LEFT_POS[3]  = { -0.032f, 1.6f, 0.0f };
	static const float RIGHT_POS[3] = {  0.032f, 1.6f, 0.0f };
	static const float AT[3]        = {  0.0f,   1.6f, -1.0f };
	struct mat4 lview, rview, lproj, rproj, product;
	int32_t     i;

	set_scene_camera(0.0f, 0.0f, 0.0f, 0.1f, 100.0f);
	xr_session_begun();

	head_pose(&lview, LEFT_POS, AT);
	head_pose(&rview, RIGHT_POS, AT);
	off_axis_proj(&lproj, -0.11f, 0.09f, -0.10f, 0.10f, 0.1f, 100.0f);
	off_axis_proj(&rproj, -0.09f, 0.11f, -0.10f, 0.10f, 0.1f, 100.0f);
	stage_pose(0, &lview, &lproj, LEFT_POS, XR_EYE_LEFT, 0, 0, 960, 1080);
	stage_pose(1, &rview, &rproj, RIGHT_POS, XR_EYE_RIGHT,
		   960, 0, 960, 1080);
	xr_frame_publish(7, 1920, 1080, 2);

	assert(g_drawn_count == 2);
	for (i = 0; i < 16; i++) {
		assert(close_enough(g_drawn[0].cam.view.m[i], lview.m[i]));
		assert(close_enough(g_drawn[0].cam.proj.m[i], lproj.m[i]));
		assert(close_enough(g_drawn[1].cam.view.m[i], rview.m[i]));
		assert(close_enough(g_drawn[1].cam.proj.m[i], rproj.m[i]));
	}
	for (i = 0; i < 3; i++) {
		assert(close_enough(g_drawn[0].cam.position[i], LEFT_POS[i]));
		assert(close_enough(g_drawn[1].cam.position[i], RIGHT_POS[i]));
	}

	/* The pair is pinned against the authored producer (#988), and its
	 * product is the product of its halves. */
	assert(g_drawn[0].cam.supplied);
	mat4_mul(&product, &g_drawn[0].cam.proj, &g_drawn[0].cam.view);
	for (i = 0; i < 16; i++)
		assert(close_enough(g_drawn[0].cam.view_proj.m[i],
				    product.m[i]));

	/* Each view is sized by its own rect, and carries where that rect is. */
	assert(g_drawn[0].width == 960 && g_drawn[0].height == 1080);
	assert(g_drawn[0].target_x == 0 && g_drawn[0].target_y == 0);
	assert(g_drawn[1].target_x == 960 && g_drawn[1].target_y == 0);

	/* The depth range the projections were built over rides along. */
	assert(close_enough(g_drawn[0].cam.near, 0.1f));
	assert(close_enough(g_drawn[0].cam.far, 100.0f));

	xr_session_ended(XR_END_NORMAL);
	assert_flat_page();
}

/*
 * The scripted camera is the stage the head pose composes onto, and the
 * composition is a translation (#994; the reasoning is at g_camera_entity_id in
 * scene_renderer.c).
 *
 * Two things have to hold at once, and they are the two halves of "the scene is
 * stable in world space when the head moves". The world moves under the viewer
 * by exactly the stage — a point the runtime reports at q is drawn where the
 * scene keeps q + stage — and the head's own ORIENTATION is untouched, because
 * the rotation an inner ear is checking against is the one thing no software
 * here may edit.
 */
static void test_the_scene_camera_is_the_stage(void)
{
	static const float POS[3]   = { 0.3f, 1.6f, -0.2f };
	static const float AT[3]    = { 1.0f, 1.4f, -3.0f };
	static const float STAGE[3] = { 5.5f, 8.5f, 10.5f };
	static const float Q[3]     = { -2.0f, 0.5f, 3.0f };
	struct mat4 view, proj;
	float       staged[3], world[3], from_q[3];
	int32_t     i;

	set_scene_camera(STAGE[0], STAGE[1], STAGE[2], 0.1f, 100.0f);
	xr_session_begun();

	head_pose(&view, POS, AT);
	off_axis_proj(&proj, -0.09f, 0.11f, -0.10f, 0.10f, 0.1f, 100.0f);
	stage_pose(0, &view, &proj, POS, XR_EYE_LEFT, 0, 0, 960, 1080);
	xr_frame_publish(7, 1920, 1080, 1);
	assert(g_drawn_count == 1);

	/* The eye stands where the scene's camera stands, plus where the user
	 * is standing in the room. */
	for (i = 0; i < 3; i++)
		assert(close_enough(g_drawn[0].cam.position[i],
				    POS[i] + STAGE[i]));

	/*
	 * A point the runtime would report at Q is drawn at Q + STAGE: pushing
	 * the world point through the composed view must land exactly where the
	 * stage-space point lands through the pose the runtime gave us.
	 */
	for (i = 0; i < 3; i++)
		world[i] = Q[i] + STAGE[i];
	mat4_transform_point(staged, &g_drawn[0].cam.view, world);
	mat4_transform_point(from_q, &view, Q);
	for (i = 0; i < 3; i++)
		assert(close_enough(staged[i], from_q[i]));

	/* And the rotation is the head's, element for element — no yaw of the
	 * scene's own look composed in, no re-derived basis. */
	for (i = 0; i < 12; i++)
		if (i % 4 != 3)
			assert(close_enough(g_drawn[0].cam.view.m[i],
					    view.m[i]));

	xr_session_ended(XR_END_NORMAL);
}

/*
 * The particle billboards' screen basis, which a supplied view has to fill
 * itself or lose (scene_view.h): eye/target/up are read as an authored look-at
 * and cross-producted into a screen frame, so they are set from THIS eye's view
 * matrix rather than left zeroed — a zeroed pair is a degenerate basis and
 * draws no particles at all.
 */
static void test_a_view_carries_a_basis_for_the_billboards(void)
{
	static const float POS[3]   = { 0.0f, 1.6f, 0.0f };
	static const float AT[3]    = { 2.0f, 0.4f, -3.0f };
	static const float STAGE[3] = { 1.0f, 2.0f, 3.0f };
	struct mat4 view, proj;
	float       fwd[3], len;
	int32_t     i;

	set_scene_camera(STAGE[0], STAGE[1], STAGE[2], 0.1f, 100.0f);
	xr_session_begun();

	head_pose(&view, POS, AT);
	off_axis_proj(&proj, -0.09f, 0.11f, -0.10f, 0.10f, 0.1f, 100.0f);
	stage_pose(0, &view, &proj, POS, XR_EYE_LEFT, 0, 0, 960, 1080);
	xr_frame_publish(7, 1920, 1080, 1);

	/* The eye is where the pair says it is, and the look points where the
	 * head is looking: target - eye is the head's forward, normalised. */
	for (i = 0; i < 3; i++)
		fwd[i] = g_drawn[0].cam.target[i] - g_drawn[0].cam.eye[i];
	len = sqrtf(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]);
	assert(close_enough(len, 1.0f));
	assert(close_enough(fwd[0], -view.m[2]));
	assert(close_enough(fwd[1], -view.m[6]));
	assert(close_enough(fwd[2], -view.m[10]));

	/* And up is the head's up, so a rolled head rolls its billboards. */
	assert(close_enough(g_drawn[0].cam.up[0], view.m[1]));
	assert(close_enough(g_drawn[0].cam.up[1], view.m[5]));
	assert(close_enough(g_drawn[0].cam.up[2], view.m[9]));

	xr_session_ended(XR_END_NORMAL);
}

/*
 * Each eye lands in its own rect of the one layer framebuffer, and the frame
 * ends on the whole layer again (#990, #994).
 *
 * This is the half of the wiring the view list cannot carry: the renderer has
 * no backend-neutral way to narrow a backbuffer to a rect, so it asks, per
 * view, and the answer is a declaration. Asserted by walking the callback the
 * way scene_renderer_tick walks it, because the declaration in force when a
 * view's graph executes is the only thing that decides which half of the
 * headset the eye comes out in.
 */
static void test_each_eye_declares_its_own_rect(void)
{
	static const float LEFT_POS[3]  = { -0.032f, 1.6f, 0.0f };
	static const float RIGHT_POS[3] = {  0.032f, 1.6f, 0.0f };
	static const float AT[3]        = {  0.0f,   1.6f, -1.0f };
	struct webgl_backbuffer_decl decl;
	struct mat4 lview, rview, proj;

	set_scene_camera(0.0f, 1.0f, 0.0f, 0.1f, 100.0f);
	xr_session_begun();

	head_pose(&lview, LEFT_POS, AT);
	head_pose(&rview, RIGHT_POS, AT);
	off_axis_proj(&proj, -0.09f, 0.11f, -0.10f, 0.10f, 0.1f, 100.0f);
	stage_pose(0, &lview, &proj, LEFT_POS, XR_EYE_LEFT, 0, 0, 960, 1080);
	stage_pose(1, &rview, &proj, RIGHT_POS, XR_EYE_RIGHT,
		   960, 0, 960, 1080);
	xr_frame_publish(7, 1920, 1080, 2);

	/* Before any view is drawn: the whole layer, which is what a pass that
	 * is not one of the views should find. */
	decl = webgl_backbuffer_declared();
	assert(decl.declared && decl.fbo == 7);
	assert(decl.x == 0 && decl.y == 0);
	assert(decl.width == 1920 && decl.height == 1080);

	g_target_fn(&g_drawn[0], g_target_user);
	decl = webgl_backbuffer_declared();
	assert(decl.fbo == 7 && decl.x == 0 && decl.y == 0);
	assert(decl.width == 960 && decl.height == 1080);

	g_target_fn(&g_drawn[1], g_target_user);
	decl = webgl_backbuffer_declared();
	assert(decl.fbo == 7 && decl.x == 960 && decl.y == 0);
	assert(decl.width == 960 && decl.height == 1080);

	/*
	 * And the end of the list gives the layer back whole. Without this the
	 * UI compositing after the scene would draw inside the right eye, and
	 * the declaration would outlive the view it was made for.
	 */
	g_target_fn(NULL, g_target_user);
	decl = webgl_backbuffer_declared();
	assert(decl.fbo == 7 && decl.x == 0 && decl.y == 0);
	assert(decl.width == 1920 && decl.height == 1080);

	xr_session_ended(XR_END_NORMAL);
	assert_flat_page();
}

/*
 * A frame with no viewer pose leaves the renderer on the view it derives from
 * the scene's camera, and the target callback keeps its hands off it. That
 * derived view's rect is the CANVAS's, and declaring it against the layer would
 * put a frame somewhere neither the canvas nor an eye — so the whole-layer
 * declaration stands and the frame goes out stretched, which is the honest
 * thing to show for the frame or two before tracking arrives.
 */
static void test_a_poseless_frame_leaves_the_layer_whole(void)
{
	struct webgl_backbuffer_decl decl;
	struct scene_view            derived;

	set_scene_camera(0.0f, 1.0f, 0.0f, 0.1f, 100.0f);
	xr_session_begun();
	xr_frame_publish(7, 1920, 1080, 0);
	assert(g_drawn_count == 0);

	memset(&derived, 0, sizeof(derived));
	derived.width  = 800;
	derived.height = 600;
	g_target_fn(&derived, g_target_user);
	g_target_fn(NULL, g_target_user);

	decl = webgl_backbuffer_declared();
	assert(decl.declared && decl.fbo == 7);
	assert(decl.width == 1920 && decl.height == 1080);

	xr_session_ended(XR_END_NORMAL);
	assert_flat_page();
}

/*
 * A view the runtime does not want drawn. XRWebGLLayer.getViewport() returns
 * nothing for such a view and the glue stages that as a rect with no area, so
 * it is dropped rather than drawn: a graph build and a full scene draw to
 * produce no pixels, ending in a declaration the backend would scissor every
 * clear to, is worse than not drawing it. It stays in the reported list — that
 * list is what the runtime said — and only the drawn list is shorter.
 */
static void test_a_view_with_no_viewport_is_not_drawn(void)
{
	static const float POS[3] = { 0.0f, 1.6f, 0.0f };
	static const float AT[3]  = { 0.0f, 1.6f, -1.0f };
	struct mat4 view, proj;

	set_scene_camera(0.0f, 1.0f, 0.0f, 0.1f, 100.0f);
	xr_session_begun();

	head_pose(&view, POS, AT);
	off_axis_proj(&proj, -0.09f, 0.11f, -0.10f, 0.10f, 0.1f, 100.0f);
	stage_pose(0, &view, &proj, POS, XR_EYE_LEFT, 0, 0, 960, 1080);
	stage_pose(1, &view, &proj, POS, XR_EYE_RIGHT, 0, 0, 0, 0);
	xr_frame_publish(7, 1920, 1080, 2);

	assert(xr_views()->count == 2);
	assert(g_drawn_count == 1);
	assert(g_drawn[0].width == 960);

	xr_session_ended(XR_END_NORMAL);
}

/*
 * The depth range a session is created with is the scene's own (#994). WebXR
 * builds every view's projection from depthNear/depthFar, so this is the one
 * number the runtime and the engine have to be told about each other — the
 * engine cannot adjust the frustum afterwards, only ask for the right one.
 */
static void test_the_depth_range_is_the_scene_camera_s(void)
{
	float near, far;

	/* No renderer to ask: WebXR's own defaults, which is what a session
	 * would have got anyway. */
	no_scene_camera();
	xr_depth_range(&near, &far);
	assert(close_enough(near, 0.1f));
	assert(close_enough(far, 1000.0f));

	/* A scene that has initialised: its range, not the browser's. */
	set_scene_camera(0.0f, 1.0f, 0.0f, 0.05f, 250.0f);
	xr_depth_range(&near, &far);
	assert(close_enough(near, 0.05f));
	assert(close_enough(far, 250.0f));

	/*
	 * A range no projection could be built from is not passed on.
	 * updateRenderState throws on it, and it is set in the same call as the
	 * layer — so a nonsense near/far would cost the session, not the depth
	 * precision.
	 */
	set_scene_camera(0.0f, 1.0f, 0.0f, 0.0f, 100.0f);
	xr_depth_range(&near, &far);
	assert(close_enough(near, 0.1f) && close_enough(far, 1000.0f));

	set_scene_camera(0.0f, 1.0f, 0.0f, 100.0f, 1.0f);
	xr_depth_range(&near, &far);
	assert(close_enough(near, 0.1f) && close_enough(far, 1000.0f));

	set_scene_camera(0.0f, 1.0f, 0.0f, 0.1f, 100.0f);
}

/*
 * The staging layout for input, written twice for the same reason the view one
 * is — as XR_IN_* here, as literals inside xr_session.c's stageInputs — and
 * pinned together here so a change that misses the JS fails a build rather than
 * shipping a headset whose controllers point at nothing.
 */
static void test_input_stage_layout_matches_the_glue(void)
{
	assert(XR_IN_ORIGIN == 0);
	assert(XR_IN_DIR == 3);
	assert(XR_IN_HAND == 6);
	assert(XR_IN_SELECT == 7);
	assert(XR_IN_STRIDE == 8);
	assert(XR_MAX_INPUT_SOURCES == 2);

	/* And the two caps the module folds together really do fold: what a
	 * runtime may report has to fit what the pointer holds. */
	assert(XR_MAX_INPUT_SOURCES <= POINTER3D_MAX);
}

/*
 * Two controllers, reaching the world-space pointer (#996). Each one arrives as
 * an aim ray and a hand, and the pair comes out in the order the runtime
 * reported them — a left hand that is published as the right one aims from the
 * wrong shoulder, which no amount of correct maths downstream can recover.
 */
static void test_two_controllers_reach_the_pointer(void)
{
	static const float LEFT_AT[3]  = { -0.2f, 1.1f, -0.3f };
	static const float RIGHT_AT[3] = {  0.2f, 1.1f, -0.3f };
	static const float FWD[3]      = {  0.0f, 0.0f, -1.0f };
	int32_t i;

	set_scene_camera(0.0f, 0.0f, 0.0f, 0.1f, 100.0f);
	xr_session_begun();

	stage_input(0, LEFT_AT, FWD, POINTER3D_HAND_LEFT, 0);
	stage_input(1, RIGHT_AT, FWD, POINTER3D_HAND_RIGHT, 0);
	xr_input_publish(2);

	assert(g_pointer_count == 2);
	for (i = 0; i < 3; i++) {
		assert(close_enough(g_pointers[0].origin[i], LEFT_AT[i]));
		assert(close_enough(g_pointers[1].origin[i], RIGHT_AT[i]));
		assert(close_enough(g_pointers[0].dir[i], FWD[i]));
	}
	assert(g_pointers[0].hand == POINTER3D_HAND_LEFT);
	assert(g_pointers[1].hand == POINTER3D_HAND_RIGHT);
	assert(g_pointers[0].click == 0 && g_pointers[1].click == 0);

	xr_session_ended(XR_END_NORMAL);
	assert_flat_page();
}

/*
 * The stage, applied to a controller exactly as it is applied to an eye (#994).
 *
 * This is the one that decides whether a headset is playable. The eyes are
 * composed onto wherever the scene put its camera; a ray composed any other way
 * — or not at all — points at a world the user is demonstrably not looking at,
 * and the error is invisible at the origin and grows with the scene, which is
 * the worst way for a bug like this to present.
 *
 * A translation moves points and leaves directions alone, so both halves are
 * asserted: the origin picks the stage up, the aim does not turn by a
 * millimetre.
 */
static void test_a_controller_ray_composes_onto_the_same_stage(void)
{
	static const float AT[3]    = { 0.10f, 1.20f, -0.35f };
	static const float DIR[3]   = { 0.0f, -0.6f, -0.8f };
	static const float STAGE[3] = { 5.5f, 8.5f, 10.5f };
	int32_t i;

	set_scene_camera(STAGE[0], STAGE[1], STAGE[2], 0.1f, 100.0f);
	xr_session_begun();

	stage_input(0, AT, DIR, POINTER3D_HAND_RIGHT, 0);
	xr_input_publish(1);

	assert(g_pointer_count == 1);
	for (i = 0; i < 3; i++) {
		assert(close_enough(g_pointers[0].origin[i],
				    AT[i] + STAGE[i]));
		assert(close_enough(g_pointers[0].dir[i], DIR[i]));
	}

	/*
	 * And the eyes drawn on the same frame agree about where that stage is,
	 * which is the property the shared xr_stage_offset exists to guarantee:
	 * an eye standing at the pose the runtime reported plus the stage.
	 */
	{
		static const float EYE[3] = { 0.0f, 1.6f, 0.0f };
		static const float LOOK[3] = { 0.0f, 1.6f, -1.0f };
		struct mat4 view, proj;

		head_pose(&view, EYE, LOOK);
		off_axis_proj(&proj, -0.09f, 0.11f, -0.10f, 0.10f, 0.1f,
			      100.0f);
		stage_pose(0, &view, &proj, EYE, XR_EYE_LEFT, 0, 0, 960, 1080);
		xr_frame_publish(7, 1920, 1080, 1);

		assert(g_drawn_count == 1);
		for (i = 0; i < 3; i++)
			assert(close_enough(g_drawn[0].cam.position[i],
					    EYE[i] + STAGE[i]));
	}

	xr_session_ended(XR_END_NORMAL);
}

/* A direction that is not unit length is normalised, because the pointer's
 * consumers are promised one and a runtime's matrix column is only ever
 * "should be". */
static void test_a_controller_ray_is_unit_length(void)
{
	static const float AT[3]  = { 0.0f, 1.0f, 0.0f };
	static const float DIR[3] = { 0.0f, 0.0f, -4.0f };

	no_scene_camera();
	xr_session_begun();

	stage_input(0, AT, DIR, POINTER3D_HAND_NONE, 0);
	xr_input_publish(1);

	assert(g_pointer_count == 1);
	assert(close_enough(g_pointers[0].dir[0], 0.0f));
	assert(close_enough(g_pointers[0].dir[1], 0.0f));
	assert(close_enough(g_pointers[0].dir[2], -1.0f));

	xr_session_ended(XR_END_NORMAL);
	set_scene_camera(0.0f, 1.0f, 0.0f, 0.1f, 100.0f);
}

/*
 * The click edge, consumed EXACTLY ONCE per press.
 *
 * The existing consumers read an edge and not a level — kruddgui clears its
 * pointer edge right after the overlays run, and a game's rules fire on the
 * change — so a press that stays true for the whole time a trigger is held
 * would move a chess piece sixty times, and a press dropped between two frames
 * would not move it at all.
 *
 * The glue's side of that contract is a TALLY, spent when it is read: this
 * drives it the way a real press does, one frame with a count and every frame
 * after it with zero, and asserts the click lives on exactly the first.
 */
static void test_the_click_edge_lasts_exactly_one_frame(void)
{
	static const float AT[3]  = { 0.0f, 1.0f, 0.0f };
	static const float FWD[3] = { 0.0f, 0.0f, -1.0f };

	xr_session_begun();

	/* Idle: no press, no edge. */
	stage_input(0, AT, FWD, POINTER3D_HAND_RIGHT, 0);
	xr_input_publish(1);
	assert(g_pointer_count == 1 && g_pointers[0].click == 0);

	/* The trigger goes down: one frame with the edge. */
	stage_input(0, AT, FWD, POINTER3D_HAND_RIGHT, 1);
	xr_input_publish(1);
	assert(g_pointers[0].click == 1);

	/*
	 * Still held. The glue spent the tally on the frame above, so every
	 * frame from here reports zero however long the finger stays down —
	 * which is what makes this an edge and not a level.
	 */
	stage_input(0, AT, FWD, POINTER3D_HAND_RIGHT, 0);
	xr_input_publish(1);
	assert(g_pointers[0].click == 0);
	xr_input_publish(1);
	assert(g_pointers[0].click == 0);

	/* And two presses inside one frame's gap are still one click frame
	 * rather than a lost press — a count cannot be set twice the way a
	 * flag can. */
	stage_input(0, AT, FWD, POINTER3D_HAND_RIGHT, 2);
	xr_input_publish(1);
	assert(g_pointers[0].click == 1);

	xr_session_ended(XR_END_NORMAL);
}

/*
 * Zero, one and two controllers, and the transitions between them. A Quest
 * user putting a controller down is normal, not exceptional: the published set
 * REPLACES the last one, so a disconnect is a smaller count and needs no
 * message of its own — and the pointer that remains is still whichever one the
 * runtime still lists, with its own hand.
 */
static void test_controllers_come_and_go_mid_session(void)
{
	static const float LEFT_AT[3]  = { -0.2f, 1.0f, 0.0f };
	static const float RIGHT_AT[3] = {  0.2f, 1.0f, 0.0f };
	static const float FWD[3]      = {  0.0f, 0.0f, -1.0f };

	no_scene_camera();
	xr_session_begun();

	/* Nothing tracked yet: a real answer, not an error. */
	xr_input_publish(0);
	assert(g_pointer_count == 0);

	/* One controller wakes up. */
	stage_input(0, RIGHT_AT, FWD, POINTER3D_HAND_RIGHT, 0);
	xr_input_publish(1);
	assert(g_pointer_count == 1);
	assert(g_pointers[0].hand == POINTER3D_HAND_RIGHT);

	/* The second joins, and the runtime lists it first. */
	stage_input(0, LEFT_AT, FWD, POINTER3D_HAND_LEFT, 0);
	stage_input(1, RIGHT_AT, FWD, POINTER3D_HAND_RIGHT, 0);
	xr_input_publish(2);
	assert(g_pointer_count == 2);
	assert(g_pointers[0].hand == POINTER3D_HAND_LEFT);

	/* The left is put down. What is left is the right one, at slot 0,
	 * still saying which hand it is. */
	stage_input(0, RIGHT_AT, FWD, POINTER3D_HAND_RIGHT, 0);
	xr_input_publish(1);
	assert(g_pointer_count == 1);
	assert(g_pointers[0].hand == POINTER3D_HAND_RIGHT);
	assert(close_enough(g_pointers[0].origin[0], RIGHT_AT[0]));

	/* And both go quiet. */
	xr_input_publish(0);
	assert(g_pointer_count == 0);

	xr_session_ended(XR_END_NORMAL);
	assert_flat_page();
	set_scene_camera(0.0f, 1.0f, 0.0f, 0.1f, 100.0f);
}

/*
 * The runtime's number is clamped, not trusted — a hand-tracking runtime lists
 * far more input sources than a pair of controllers — and a source with no
 * direction is dropped rather than published as a ray that points nowhere,
 * which the pick would refuse and the debug rod could not be oriented from.
 */
static void test_input_clamps_and_drops_what_it_cannot_use(void)
{
	static const float AT[3]   = { 0.0f, 1.0f, 0.0f };
	static const float FWD[3]  = { 0.0f, 0.0f, -1.0f };
	static const float ZERO[3] = { 0.0f, 0.0f, 0.0f };

	xr_session_begun();

	stage_input(0, AT, FWD, POINTER3D_HAND_LEFT, 0);
	stage_input(1, AT, FWD, POINTER3D_HAND_RIGHT, 0);
	xr_input_publish(XR_MAX_INPUT_SOURCES + 5);
	assert(g_pointer_count == XR_MAX_INPUT_SOURCES);

	xr_input_publish(-3);
	assert(g_pointer_count == 0);

	/* A tracked source with no aim is not a pointer: the one beside it
	 * still is, and moves down into its place. */
	stage_input(0, AT, ZERO, POINTER3D_HAND_LEFT, 0);
	stage_input(1, AT, FWD, POINTER3D_HAND_RIGHT, 0);
	xr_input_publish(2);
	assert(g_pointer_count == 1);
	assert(g_pointers[0].hand == POINTER3D_HAND_RIGHT);

	/* An unrecognised handedness is "none" rather than whatever number the
	 * runtime happened to send. */
	stage_input(0, AT, FWD, 77, 0);
	xr_input_publish(1);
	assert(g_pointers[0].hand == POINTER3D_HAND_NONE);

	xr_session_ended(XR_END_NORMAL);
}

/*
 * Outside a session nothing points at anything. The glue is not the only caller
 * this export could ever have, and a pointer published with no session behind
 * it would leave a rod in the flat page's scene with nothing moving it.
 */
static void test_no_session_means_no_pointer(void)
{
	static const float AT[3]  = { 0.0f, 1.0f, 0.0f };
	static const float FWD[3] = { 0.0f, 0.0f, -1.0f };

	assert(xr_session_active() == 0);
	stage_input(0, AT, FWD, POINTER3D_HAND_RIGHT, 1);
	xr_input_publish(1);
	assert(g_pointer_count == 0);

	/*
	 * Entering a session clears whatever the last one left, so the first
	 * frame of a session never opens on a stale hand.
	 */
	xr_session_begun();
	assert(g_pointer_count == -1);
	xr_session_ended(XR_END_NORMAL);
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

	RUN(a_stereo_frame_reaches_the_renderer);
	RUN(the_scene_camera_is_the_stage);
	RUN(a_view_carries_a_basis_for_the_billboards);
	RUN(each_eye_declares_its_own_rect);
	RUN(a_poseless_frame_leaves_the_layer_whole);
	RUN(a_view_with_no_viewport_is_not_drawn);
	RUN(the_depth_range_is_the_scene_camera_s);

	RUN(input_stage_layout_matches_the_glue);
	RUN(no_session_means_no_pointer);
	RUN(two_controllers_reach_the_pointer);
	RUN(a_controller_ray_composes_onto_the_same_stage);
	RUN(a_controller_ray_is_unit_length);
	RUN(the_click_edge_lasts_exactly_one_frame);
	RUN(controllers_come_and_go_mid_session);
	RUN(input_clamps_and_drops_what_it_cannot_use);

	printf("%d/%d tests passed\n", tests_passed, tests_run);
	return tests_passed == tests_run ? 0 : 1;
}
