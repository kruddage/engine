/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * gizmo — the subsystem face: the vtable the bridge drives, and the kruddgui
 * overlay that draws the handles, the grid and the axis indicator.
 *
 * The arithmetic is gizmo.c and it is pure. This file is everything that has to
 * touch the rest of the engine — the world, the camera, the viewport, and
 * kruddgui's quad batch — and it is deliberately thin, because none of it can
 * be tested without a browser.
 *
 * ## Where the drawing goes, and why not the frame graph
 *
 * #949's note says editor drawing goes through `render/frame_graph`, and #944's
 * Q4 decision says the editor's gizmos render on kruddgui's surface. Both are
 * satisfied at once, because that is where kruddgui already draws: its quad
 * batch is submitted inside the render graph's pass, not on the side. What the
 * note rules out is a second, private path to the GPU, and registering an
 * overlay is the opposite of that — it is the seam `kruddgui_api.h` describes
 * itself as existing for, whose only tenant until now was ui/viewport.
 *
 * ## What is engine-drawn and what is not
 *
 * Handles, grid and axis indicator: here, in the canvas, over the scene.
 * **Selection: not here.** `render/scene_renderer` has drawn a real
 * silhouette-and-edge-detect outline through the frame graph since long before
 * this issue, gated on editor mode — it is depth-correct, it is already tested,
 * and painting a second selection cue on the quad batch would be a worse one
 * drawn twice.
 *
 * wasm-only for the drawing, like ui/viewport: native builds host no canvas and
 * no kruddgui pointer. The vtable and the state are built either way so that
 * the bridge's contract does not change shape between targets.
 */
#include "gizmo.h"

#include "subsystem.h"
#include "subsystem_manager.h"
#include "log_api.h"
#include "log_level.h"

#include <math.h>
#include <string.h>

static const struct log_api           *g_log;
static const struct subsystem_manager *g_mgr;

/*
 * The mode, the snap increments and the grid. Engine state rather than editor
 * state, because two things read them — the overlay below and the editor's own
 * mode buttons — and a second copy is a second thing to disagree.
 *
 * The defaults are the ones a reader wants on first launch: translate, a grid
 * at one unit, and snapping off, so nothing quantizes an edit until it is asked
 * for.
 */
static int32_t          g_mode = GIZMO_MODE_TRANSLATE;
static struct gizmo_snap g_snap;
static int32_t          g_grid_shown = 1;
static float            g_grid_spacing = 1.0f;
static struct gizmo_drag g_drag;

#ifdef __EMSCRIPTEN__
#include "kruddgui_api.h"
#include "camera_api.h"
#include "entity_api.h"
#include "viewport_api.h"
#include "world.h"

static const struct kruddgui_api *g_kgui;    /* NULL until kruddgui is up */
static const struct camera_api   *g_camera;
static const struct entity_api   *g_scene;
static const struct viewport_api *g_viewport;
static int                        g_registered;

/* Editor mode — the same flag scene_renderer's outline follows. */
int krudd_editor_mode(void);

/* Handle colours: x red, y green, z blue, and the grabbed one lit. */
static const float AXIS_RGB[3][3] = {
	{ 0.91f, 0.30f, 0.33f },
	{ 0.42f, 0.80f, 0.36f },
	{ 0.33f, 0.55f, 0.95f },
};

/*
 * Assemble this frame's gizmo frame for the selected entity.
 *
 * Returns 0 — and draws nothing — whenever any part of it is missing: no
 * selection, no camera, a viewport of zero size. Every caller treats that as
 * "there is no gizmo this frame" rather than as an error, because all of those
 * are ordinary states rather than faults.
 */
static int32_t current_frame(struct gizmo_frame *out)
{
	const struct world *w;
	int32_t             id, parent;

	if (!g_camera || !g_scene || !g_viewport)
		return 0;
	id = g_scene->get_selected ? g_scene->get_selected() : -1;
	if (id < 0)
		return 0;
	w = g_scene->get_world();
	if (!w || (uint32_t)id >= w->count || !w->alive[id])
		return 0;

	memset(out, 0, sizeof(*out));
	g_camera->get_view_proj(&out->view_proj);
	g_camera->get_eye(out->eye);
	g_viewport->get_size(&out->width, &out->height);
	if (out->width <= 0.0f || out->height <= 0.0f)
		return 0;

	out->pivot     = w->local[id];
	out->origin[0] = w->world_xform[id].position[0];
	out->origin[1] = w->world_xform[id].position[1];
	out->origin[2] = w->world_xform[id].position[2];

	gizmo_frame_identity_basis(out);
	parent = w->parent[id];
	if (parent >= 0 && (uint32_t)parent < w->count && w->alive[parent]) {
		const struct transform *p = &w->world_xform[parent];
		struct mat4             m;
		int                     a;

		/*
		 * The parent's rotation, read out of its model matrix's
		 * columns and re-normalized. Going through mat4_from_transform
		 * rather than rotating three unit vectors by the quaternion
		 * keeps one implementation of "what does this transform do to
		 * a direction" in the tree; the column lengths are the
		 * parent's scale, which is exactly what axis_scale wants.
		 */
		mat4_from_transform(&m, p);
		for (a = 0; a < 3; a++) {
			float len;

			out->basis[a][0] = m.m[a * 4 + 0];
			out->basis[a][1] = m.m[a * 4 + 1];
			out->basis[a][2] = m.m[a * 4 + 2];
			len = out->basis[a][0] * out->basis[a][0] +
			      out->basis[a][1] * out->basis[a][1] +
			      out->basis[a][2] * out->basis[a][2];
			len = len > 1e-12f ? sqrtf(len) : 0.0f;
			out->axis_scale[a] = len > 0.0f ? len : 1.0f;
			if (len > 0.0f) {
				out->basis[a][0] /= len;
				out->basis[a][1] /= len;
				out->basis[a][2] /= len;
			}
		}
	}
	return 1;
}

/* ------------------------------------------------------------------ *
 * Drawing
 * ------------------------------------------------------------------ */

/*
 * A world-space line, clipped to "both ends are in front of the camera".
 *
 * Segment clipping against the near plane would be the complete answer and it
 * is not worth it here: every line this overlay draws is either a grid line
 * (there are many, and dropping the ones that straddle the eye plane loses a
 * segment at the very edge of view) or a gizmo handle (short, and centred on a
 * point that has to be visible for the gizmo to be usable at all).
 */
static void line3(const struct gizmo_frame *f, const float a[3],
		  const float b[3], float width,
		  float r, float g, float bl, float alpha)
{
	float pa[2], pb[2];

	if (!gizmo_project(f, a, pa) || !gizmo_project(f, b, pb))
		return;
	g_kgui->line(pa[0], pa[1], pb[0], pb[1], width, r, g, bl, alpha);
}

/*
 * The ground grid, on the world's y = 0 plane, centred on the camera's target
 * rather than on the origin so it does not run out from under a reader who
 * panned away from it.
 */
static void draw_grid(const struct gizmo_frame *f)
{
	float   centre[3];
	float   step = g_grid_spacing;
	int     i;
	const int HALF = 20;   /* lines each side of centre */

	if (!g_grid_shown || step <= 0.0f)
		return;

	/*
	 * Snapped to the grid itself. An unsnapped centre makes the lines
	 * shimmer as the camera moves, because each one is redrawn at a
	 * slightly different world position every frame.
	 */
	centre[0] = step * (float)((int)(f->origin[0] / step));
	centre[1] = 0.0f;
	centre[2] = step * (float)((int)(f->origin[2] / step));

	for (i = -HALF; i <= HALF; i++) {
		float a[3], b[3];
		float shade = i == 0 ? 0.42f : 0.22f;

		a[0] = centre[0] + (float)i * step;
		a[1] = 0.0f;
		a[2] = centre[2] - (float)HALF * step;
		b[0] = a[0];
		b[1] = 0.0f;
		b[2] = centre[2] + (float)HALF * step;
		line3(f, a, b, 1.0f, shade, shade, shade, 0.55f);

		a[0] = centre[0] - (float)HALF * step;
		a[2] = centre[2] + (float)i * step;
		b[0] = centre[0] + (float)HALF * step;
		b[2] = a[2];
		line3(f, a, b, 1.0f, shade, shade, shade, 0.55f);
	}
}

/*
 * The axis indicator: three short bars in the corner, drawn from the camera's
 * own orientation rather than projected from a world point, so it keeps its
 * size and its place whatever the scene is doing.
 *
 * The directions come out of the view·projection's rows, which is where the
 * camera's basis already lives — a separate copy of it here would be a second
 * thing to keep in step with the renderer's camera.
 */
static void draw_axis_indicator(const struct gizmo_frame *f)
{
	const float BAR  = 26.0f;
	const float PAD  = 40.0f;
	float       cx   = PAD;
	float       cy   = f->height - PAD;
	int         a;

	for (a = 0; a < 3; a++) {
		/*
		 * The screen-space direction of world axis `a`: the first two
		 * rows of view·projection give how x and y move per unit of it.
		 * Normalized, so a long axis and a short one draw the same
		 * length and only the direction reads.
		 */
		float dx = f->view_proj.m[a * 4 + 0];
		float dy = -f->view_proj.m[a * 4 + 1];
		float len = sqrtf(dx * dx + dy * dy);

		if (len <= 1e-6f)
			continue;
		dx = dx / len * BAR;
		dy = dy / len * BAR;
		g_kgui->line(cx, cy, cx + dx, cy + dy, 2.0f,
			     AXIS_RGB[a][0], AXIS_RGB[a][1], AXIS_RGB[a][2],
			     0.95f);
		g_kgui->circle(cx + dx, cy + dy, 3.0f,
			       AXIS_RGB[a][0], AXIS_RGB[a][1], AXIS_RGB[a][2],
			       0.95f);
	}
}

/*
 * One rotation ring, as the polyline gizmo_hit tests against.
 *
 * kruddgui_api carries a `ring` primitive and this deliberately does not use
 * it: that draws a screen-space circle, and these are world-space circles seen
 * at an angle. Drawing a circle where the hit-test walks an ellipse is exactly
 * the drift #813 warns about, in miniature — so both walk gizmo_ring_point.
 */
static void draw_ring(const struct gizmo_frame *f, enum gizmo_axis axis,
		      float width, float r, float g, float b, float alpha)
{
	float   prev[2];
	int32_t have_prev = 0;
	int32_t i;

	for (i = 0; i <= GIZMO_RING_STEPS; i++) {
		float world[3], now[2];

		gizmo_ring_point(f, axis, i, world);
		if (!gizmo_project(f, world, now)) {
			have_prev = 0;
			continue;
		}
		if (have_prev)
			g_kgui->line(prev[0], prev[1], now[0], now[1], width,
				     r, g, b, alpha);
		prev[0]   = now[0];
		prev[1]   = now[1];
		have_prev = 1;
	}
}

/* The handles for the live mode. */
static void draw_handles(const struct gizmo_frame *f)
{
	float tips[3][3];
	float centre[2], tip[2];
	int   a;

	if (g_mode == GIZMO_MODE_NONE)
		return;
	if (!gizmo_project(f, f->origin, centre))
		return;

	gizmo_handle_tips(f, tips);
	for (a = 0; a < 3; a++) {
		float alpha = 1.0f;
		float width = 2.5f;

		/*
		 * The grabbed handle is thicker and the others fade. Feedback
		 * that a drag took, which matters most in the case the reader
		 * grabbed a different axis than they meant to.
		 */
		if (g_drag.active) {
			if ((int)g_drag.axis == a) {
				width = 4.0f;
			} else {
				alpha = 0.28f;
			}
		}

		if (g_mode == GIZMO_MODE_ROTATE) {
			draw_ring(f, (enum gizmo_axis)a, width,
				  AXIS_RGB[a][0], AXIS_RGB[a][1],
				  AXIS_RGB[a][2], alpha);
			continue;
		}

		if (!gizmo_project(f, tips[a], tip))
			continue;
		g_kgui->line(centre[0], centre[1], tip[0], tip[1], width,
			     AXIS_RGB[a][0], AXIS_RGB[a][1], AXIS_RGB[a][2],
			     alpha);
		/* A square cap for scale, a disc for translate — the shape is
		 * how the mode reads at a glance without a legend. */
		if (g_mode == GIZMO_MODE_SCALE)
			g_kgui->rect(tip[0] - 4.0f, tip[1] - 4.0f, 8.0f, 8.0f,
				     AXIS_RGB[a][0], AXIS_RGB[a][1],
				     AXIS_RGB[a][2], alpha);
		else
			g_kgui->circle(tip[0], tip[1], 4.5f,
				       AXIS_RGB[a][0], AXIS_RGB[a][1],
				       AXIS_RGB[a][2], alpha);
	}

	if (g_mode == GIZMO_MODE_SCALE)
		g_kgui->circle(centre[0], centre[1], 5.0f,
			       0.92f, 0.92f, 0.92f,
			       g_drag.active && g_drag.axis == GIZMO_AXIS_ALL
				       ? 1.0f
				       : 0.7f);
}

/*
 * The overlay. Grid and axis indicator whenever the editor is up; handles only
 * when something is selected.
 *
 * It draws and does not read the pointer. That is #949's one-owner rule: in
 * editor mode the editor owns the pointer over the canvas and reaches the
 * gizmo through the bridge, and an overlay that also read kruddgui's pointer
 * would be the second owner the rule exists to prevent.
 */
static void gizmo_overlay(void *userdata)
{
	struct gizmo_frame f;

	(void)userdata;
	if (!g_kgui || !krudd_editor_mode())
		return;
	if (!current_frame(&f)) {
		/*
		 * No selection is still an editor. Build a frame with the
		 * camera in it and no pivot, so the grid and the indicator
		 * survive an empty selection — a viewport that loses its
		 * ground plane the moment you deselect is disorienting, and
		 * that is precisely when a reader is looking for it.
		 */
		if (!g_camera || !g_viewport)
			return;
		memset(&f, 0, sizeof(f));
		g_camera->get_view_proj(&f.view_proj);
		g_camera->get_eye(f.eye);
		g_viewport->get_size(&f.width, &f.height);
		if (f.width <= 0.0f || f.height <= 0.0f)
			return;
		gizmo_frame_identity_basis(&f);
		f.origin[0] = f.eye[0];
		f.origin[2] = f.eye[2];
		draw_grid(&f);
		draw_axis_indicator(&f);
		return;
	}
	draw_grid(&f);
	draw_handles(&f);
	draw_axis_indicator(&f);
}
#endif /* __EMSCRIPTEN__ */

/* ------------------------------------------------------------------ *
 * The vtable
 *
 * Built on both targets. A native build has no camera and no world to build a
 * frame from, so begin/move answer "nothing grabbed" — which is the honest
 * answer rather than a stub, and it keeps the bridge's shape identical between
 * the two.
 * ------------------------------------------------------------------ */

#ifndef __EMSCRIPTEN__
static int32_t current_frame(struct gizmo_frame *out)
{
	(void)out;
	return 0;
}
#endif

static void api_set_mode(int32_t mode)
{
	if (mode < GIZMO_MODE_NONE || mode > GIZMO_MODE_SCALE)
		return;
	/* A mode change during a drag would leave the drag solving for handles
	 * that are no longer drawn. Close it first. */
	if (g_drag.active && mode != g_mode)
		gizmo_end(&g_drag);
	g_mode = mode;
}

static int32_t api_get_mode(void)
{
	return g_mode;
}

static void api_set_snap(float translate, float rotate_degrees, float scale)
{
	/* Negative is not "off" — off is zero, and a negative increment would
	 * round the wrong way. Clamped rather than rejected so a slider that
	 * overshoots does not need a guard at every call site. */
	g_snap.translate      = translate > 0.0f ? translate : 0.0f;
	g_snap.rotate_degrees = rotate_degrees > 0.0f ? rotate_degrees : 0.0f;
	g_snap.scale          = scale > 0.0f ? scale : 0.0f;
}

static void api_get_snap(struct gizmo_snap *out)
{
	if (out)
		*out = g_snap;
}

static int32_t api_begin(float sx, float sy)
{
	struct gizmo_frame f;

	gizmo_end(&g_drag);
	if (!current_frame(&f))
		return GIZMO_AXIS_NONE;
	if (!gizmo_begin(&g_drag, &f, (enum gizmo_mode)g_mode, sx, sy))
		return GIZMO_AXIS_NONE;
	return (int32_t)g_drag.axis;
}

static int32_t api_move(float sx, float sy, struct transform *out)
{
	struct gizmo_frame f;

	if (!g_drag.active || !out)
		return 0;
	if (!current_frame(&f))
		return 0;
	return gizmo_drag_to(&g_drag, &f, &g_snap, sx, sy, out);
}

static void api_end(void)
{
	gizmo_end(&g_drag);
}

static int32_t api_axis(void)
{
	return g_drag.active ? (int32_t)g_drag.axis : GIZMO_AXIS_NONE;
}

static void api_set_grid(int32_t shown, float spacing)
{
	g_grid_shown = shown ? 1 : 0;
	if (spacing > 0.0f)
		g_grid_spacing = spacing;
}

static int32_t api_grid_shown(void)
{
	return g_grid_shown;
}

static float api_grid_spacing(void)
{
	return g_grid_spacing;
}

static const struct gizmo_api g_api = {
	api_set_mode,
	api_get_mode,
	api_set_snap,
	api_get_snap,
	api_begin,
	api_move,
	api_end,
	api_axis,
	api_set_grid,
	api_grid_shown,
	api_grid_spacing,
};

static void gizmo_init(void)
{
	gizmo_end(&g_drag);
	if (g_log)
		g_log->write(LOG_LEVEL_INFO, "gizmo: init");
}

static void gizmo_tick(void)
{
#ifdef __EMSCRIPTEN__
	if (g_registered)
		return;

	/*
	 * kruddgui registers after this plugin, so its api is not up at
	 * plugin_entry. Resolve it lazily and register the overlay once it is
	 * — the same shape ui/viewport uses, for the same reason.
	 */
	g_kgui = subsystem_manager_get_api(g_mgr, "kruddgui");
	if (!g_kgui)
		return;
	g_kgui->register_overlay(gizmo_overlay, NULL);
	g_registered = 1;
	if (g_log)
		g_log->write(LOG_LEVEL_INFO, "gizmo: overlay registered");
#endif
}

static void gizmo_shutdown(void)
{
	gizmo_end(&g_drag);
	if (g_log)
		g_log->write(LOG_LEVEL_INFO, "gizmo: shutdown");
}

static const struct subsystem desc = {
	.name     = "gizmo",
	.api      = &g_api,
	.init     = gizmo_init,
	.tick     = gizmo_tick,
	.shutdown = gizmo_shutdown,
};

void gizmo_plugin_entry(struct subsystem_manager *mgr)
{
	g_mgr = mgr;
	g_log = subsystem_manager_get_api(mgr, "log");
#ifdef __EMSCRIPTEN__
	g_camera   = subsystem_manager_get_api(mgr, "camera");
	g_scene    = subsystem_manager_get_api(mgr, "scene");
	g_viewport = subsystem_manager_get_api(mgr, "viewport");
#endif
	subsystem_manager_register(mgr, &desc);
}
