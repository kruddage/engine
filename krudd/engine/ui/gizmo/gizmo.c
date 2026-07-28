/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * gizmo — the transform arithmetic. See include/gizmo.h for what this is and
 * why it lives in C; this file is projection, hit-testing and the three drag
 * solves, in that order.
 *
 * Nothing here reads a subsystem, a world or a pointer. Every function takes
 * its whole frame as an argument, which is what lets gizmo_test drive the maths
 * against a synthetic camera with no GPU — the issue's instruction, and the
 * right one: gizmo arithmetic is easy to get subtly wrong and miserable to
 * debug by eye, because a wrong answer still looks like a gizmo.
 */
#include "gizmo.h"

#include <math.h>
#include <string.h>

#define GIZMO_PI	3.14159265358979323846f

/* ------------------------------------------------------------------ *
 * Small vector helpers
 *
 * Local rather than reached for from base/math: these are three-line
 * operations on float[3], the math library exposes matrix and ray work rather
 * than a vector algebra, and importing one for the other would be a dependency
 * bought for `sub`.
 * ------------------------------------------------------------------ */

static void v_sub(float out[3], const float a[3], const float b[3])
{
	out[0] = a[0] - b[0];
	out[1] = a[1] - b[1];
	out[2] = a[2] - b[2];
}

static float v_len(const float a[3])
{
	return sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
}

/* Unit-length in place. Leaves a zero vector alone rather than producing NaN. */
static void v_norm(float a[3])
{
	float n = v_len(a);

	if (n <= 1e-6f)
		return;
	a[0] /= n;
	a[1] /= n;
	a[2] /= n;
}

/* ------------------------------------------------------------------ *
 * Projection
 * ------------------------------------------------------------------ */

int32_t gizmo_project(const struct gizmo_frame *f, const float world[3],
		      float out[2])
{
	const float *m;
	float        x, y, w;

	if (!f || !world || !out || f->width <= 0.0f || f->height <= 0.0f)
		return 0;

	m = f->view_proj.m;
	x = m[0] * world[0] + m[4] * world[1] + m[8]  * world[2] + m[12];
	y = m[1] * world[0] + m[5] * world[1] + m[9]  * world[2] + m[13];
	w = m[3] * world[0] + m[7] * world[1] + m[11] * world[2] + m[15];

	/*
	 * Behind the eye, or on the plane through it. Rejected rather than
	 * divided: a negative w mirrors the point through the origin, and a
	 * handle drawn at its mirror is one the reader can grab where it is
	 * not.
	 */
	if (w <= 1e-6f)
		return 0;

	/* NDC to pixels, y flipped — the screen convention ray_from_screen
	 * takes, so a projected handle and a picked pixel mean the same thing. */
	out[0] = (x / w * 0.5f + 0.5f) * f->width;
	out[1] = (0.5f - y / w * 0.5f) * f->height;
	return 1;
}

/*
 * World units per pixel at the origin.
 *
 * Derived from the eye distance rather than from the projection matrix, and
 * deliberately: the same expression works for whatever fov the camera holds,
 * and the constant below is a tuning number rather than a geometric claim. What
 * matters is that it scales linearly with distance, which is what keeps a
 * handle the same size on screen as the camera dollies.
 */
float gizmo_screen_scale(const struct gizmo_frame *f)
{
	float to_eye[3];
	float dist;

	if (!f || f->width <= 0.0f || f->height <= 0.0f)
		return 0.0f;
	v_sub(to_eye, f->eye, f->origin);
	dist = v_len(to_eye);
	if (dist <= 1e-4f)
		return 0.0f;
	/*
	 * One arm spans GIZMO_ARM_PIXELS at any distance. Height rather than
	 * width, so a wide dock does not grow the gizmo — the vertical field of
	 * view is the one the projection holds fixed.
	 */
	return dist / f->height;
}

void gizmo_handle_tips(const struct gizmo_frame *f, float out[3][3])
{
	float arm;
	int   a, c;

	if (!f || !out)
		return;
	arm = gizmo_screen_scale(f) * GIZMO_ARM_PIXELS;
	for (a = 0; a < 3; a++)
		for (c = 0; c < 3; c++)
			out[a][c] = f->origin[c] + f->basis[a][c] * arm;
}

void gizmo_frame_identity_basis(struct gizmo_frame *f)
{
	int a, c;

	if (!f)
		return;
	for (a = 0; a < 3; a++) {
		f->axis_scale[a] = 1.0f;
		for (c = 0; c < 3; c++)
			f->basis[a][c] = a == c ? 1.0f : 0.0f;
	}
}

void gizmo_ring_point(const struct gizmo_frame *f, enum gizmo_axis axis,
		      int32_t step, float out[3])
{
	/* The two basis vectors spanning the plane the ring lies in. */
	int   u = (axis + 1) % 3;
	int   v = (axis + 2) % 3;
	float radius, theta, cu, cv;
	int   c;

	if (!f || !out || axis < GIZMO_AXIS_X || axis > GIZMO_AXIS_Z)
		return;
	radius = gizmo_screen_scale(f) * GIZMO_ARM_PIXELS;
	theta  = 2.0f * GIZMO_PI * (float)step / (float)GIZMO_RING_STEPS;
	cu     = cosf(theta) * radius;
	cv     = sinf(theta) * radius;
	for (c = 0; c < 3; c++)
		out[c] = f->origin[c] + f->basis[u][c] * cu +
			 f->basis[v][c] * cv;
}

/* ------------------------------------------------------------------ *
 * Hit-testing
 * ------------------------------------------------------------------ */

/* Distance from p to the segment ab, all in screen pixels. */
static float point_segment_distance(const float p[2], const float a[2],
				    const float b[2])
{
	float abx = b[0] - a[0], aby = b[1] - a[1];
	float apx = p[0] - a[0], apy = p[1] - a[1];
	float len2 = abx * abx + aby * aby;
	float t, dx, dy;

	if (len2 <= 1e-6f) {
		dx = apx;
		dy = apy;
		return sqrtf(dx * dx + dy * dy);
	}
	t = (apx * abx + apy * aby) / len2;
	if (t < 0.0f)
		t = 0.0f;
	else if (t > 1.0f)
		t = 1.0f;
	dx = apx - abx * t;
	dy = apy - aby * t;
	return sqrtf(dx * dx + dy * dy);
}

/*
 * The rotate mode's rings, tested against the polyline that draws them.
 *
 * Walked rather than solved as an ellipse, and that is the point: the overlay
 * draws these same GIZMO_RING_STEPS segments, so the pixels a reader aims at
 * and the geometry that answers them are one walk rather than two descriptions
 * of a circle that can drift apart (#813).
 *
 * A ring seen edge-on collapses to a line, and that falls out correctly — the
 * polyline degenerates with it, so the ring stays grabbable exactly along the
 * pixels it is still drawn on.
 */
static float ring_distance(const struct gizmo_frame *f, enum gizmo_axis axis,
			   const float p[2])
{
	float   best = 1e30f;
	float   prev[2];
	int32_t have_prev = 0;
	int32_t i;

	for (i = 0; i <= GIZMO_RING_STEPS; i++) {
		float world[3], now[2], d;

		gizmo_ring_point(f, axis, i, world);
		if (!gizmo_project(f, world, now)) {
			have_prev = 0;
			continue;
		}
		if (have_prev) {
			d = point_segment_distance(p, prev, now);
			if (d < best)
				best = d;
		}
		prev[0]   = now[0];
		prev[1]   = now[1];
		have_prev = 1;
	}
	return best;
}

enum gizmo_axis gizmo_hit(const struct gizmo_frame *f, enum gizmo_mode mode,
			  float sx, float sy)
{
	float           tips[3][3];
	float           centre[2], tip[2], p[2];
	float           best = GIZMO_GRAB_PIXELS;
	enum gizmo_axis found = GIZMO_AXIS_NONE;
	int             a;

	if (!f || mode == GIZMO_MODE_NONE)
		return GIZMO_AXIS_NONE;
	if (!gizmo_project(f, f->origin, centre))
		return GIZMO_AXIS_NONE;

	p[0] = sx;
	p[1] = sy;

	/*
	 * The centre handle, scale only. Tested first and separately because it
	 * overlaps all three arms at their base — checking it after them would
	 * make it unreachable, since an arm is always at distance 0 there.
	 */
	if (mode == GIZMO_MODE_SCALE) {
		float dx = p[0] - centre[0], dy = p[1] - centre[1];

		if (sqrtf(dx * dx + dy * dy) <= GIZMO_GRAB_PIXELS)
			return GIZMO_AXIS_ALL;
	}

	/*
	 * The rotate mode's dead centre. Tested once, before the rings, rather
	 * than per-ring: an edge-on ring passes through the pivot, so without
	 * this the hub belongs to whichever ring is most edge-on — the one
	 * whose rotation the reader can least use.
	 */
	if (mode == GIZMO_MODE_ROTATE) {
		float dx = p[0] - centre[0], dy = p[1] - centre[1];

		/*
		 * In pixels directly. A ring's world radius is exactly the
		 * arm's, and the arm is sized to span GIZMO_ARM_PIXELS on
		 * screen — so the nominal ring is that many pixels across and
		 * the dead zone is a fraction of it. Measuring a particular
		 * ring instead would make the hub's size depend on which of
		 * them happened to be face-on.
		 */
		if (sqrtf(dx * dx + dy * dy) <
		    GIZMO_RING_DEAD_ZONE * GIZMO_ARM_PIXELS)
			return GIZMO_AXIS_NONE;
	}

	gizmo_handle_tips(f, tips);
	for (a = 0; a < 3; a++) {
		float d;

		if (mode == GIZMO_MODE_ROTATE) {
			d = ring_distance(f, (enum gizmo_axis)a, p);
		} else {
			if (!gizmo_project(f, tips[a], tip))
				continue;
			d = point_segment_distance(p, centre, tip);
		}
		if (d < best) {
			best  = d;
			found = (enum gizmo_axis)a;
		}
	}
	return found;
}

/* ------------------------------------------------------------------ *
 * The drag solves
 *
 * Each mode maps the pointer to one scalar, and the transform is a function of
 * (now - start) on that scalar. Absolute against the drag's origin rather than
 * integrated from per-frame deltas: an integrated drag drifts under snapping —
 * every frame rounds, and the rounding adds up — and it cannot be made exact
 * again by moving the pointer back where it started, which is the thing a
 * reader tries first when a drag goes wrong.
 * ------------------------------------------------------------------ */

/* Round to the nearest multiple of `step`. A non-positive step does not snap. */
static float snap_to(float value, float step)
{
	if (step <= 0.0f)
		return value;
	return roundf(value / step) * step;
}

/*
 * How far along the handle the pointer is, in the *local* units the pivot's
 * position is expressed in.
 *
 * Screen-space projection of the pointer onto the handle's screen direction,
 * scaled back into world units by the handle's own screen length, then divided
 * by the parent's scale along that axis to land in local units. That is the
 * standard solve and it degrades gracefully: an axis pointing at the camera has
 * a near-zero screen length, and returning 0 there means the drag holds rather
 * than flying off, which is what dividing by it would do.
 */
static int32_t axis_distance(const struct gizmo_frame *f, enum gizmo_axis axis,
			     float sx, float sy, float *out)
{
	float tips[3][3];
	float centre[2], tip[2];
	float ax, ay, len2, per_local;

	if (!gizmo_project(f, f->origin, centre))
		return 0;
	gizmo_handle_tips(f, tips);
	if (!gizmo_project(f, tips[axis], tip))
		return 0;

	ax   = tip[0] - centre[0];
	ay   = tip[1] - centre[1];
	len2 = ax * ax + ay * ay;
	if (len2 <= 1.0f)
		return 0;

	/* A parent scaled to nothing on this axis leaves no motion to solve. */
	per_local = f->axis_scale[axis];
	if (per_local <= 1e-5f && per_local >= -1e-5f)
		return 0;

	/*
	 * The dot product gives the pointer's position along the handle as a
	 * fraction of the arm; the arm is GIZMO_ARM_PIXELS * screen_scale world
	 * units long, so the product of the two is world units.
	 */
	*out = ((sx - centre[0]) * ax + (sy - centre[1]) * ay) / len2 *
	       (GIZMO_ARM_PIXELS * gizmo_screen_scale(f)) / per_local;
	return 1;
}

/* The pointer's angle about the pivot, in radians. */
static int32_t screen_angle(const struct gizmo_frame *f, float sx, float sy,
			    float *out)
{
	float centre[2];
	float dx, dy;

	if (!gizmo_project(f, f->origin, centre))
		return 0;
	dx = sx - centre[0];
	dy = sy - centre[1];
	/* Too close to the centre to have an angle at all. */
	if (dx * dx + dy * dy < 4.0f)
		return 0;
	*out = atan2f(dy, dx);
	return 1;
}

/* The pointer's distance from the pivot, in pixels. */
static int32_t screen_radius(const struct gizmo_frame *f, float sx, float sy,
			     float *out)
{
	float centre[2];
	float dx, dy;

	if (!gizmo_project(f, f->origin, centre))
		return 0;
	dx   = sx - centre[0];
	dy   = sy - centre[1];
	*out = sqrtf(dx * dx + dy * dy);
	/* A press exactly on the pivot gives no ratio to scale by. */
	return *out > 1.0f;
}

/* Hamilton product, xyzw. The rotation composes on the left: world-axis. */
static void quat_mul(float out[4], const float a[4], const float b[4])
{
	float x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
	float y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
	float z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
	float w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];

	out[0] = x;
	out[1] = y;
	out[2] = z;
	out[3] = w;
}

/*
 * Which way the axis points relative to the camera, +1 or -1.
 *
 * A rotation ring seen from behind turns the other way on screen, so a drag
 * that ignored this would rotate backwards for exactly half the orbits a reader
 * can put the camera in — and "sometimes backwards" is the hardest gizmo bug to
 * report.
 */
static float axis_facing(const struct gizmo_frame *f, enum gizmo_axis axis)
{
	float to_eye[3];
	float dot;

	v_sub(to_eye, f->eye, f->origin);
	v_norm(to_eye);
	dot = to_eye[0] * f->basis[axis][0] + to_eye[1] * f->basis[axis][1] +
	      to_eye[2] * f->basis[axis][2];
	return dot < 0.0f ? -1.0f : 1.0f;
}

int32_t gizmo_begin(struct gizmo_drag *d, const struct gizmo_frame *f,
		    enum gizmo_mode mode, float sx, float sy)
{
	enum gizmo_axis axis;
	float           value = 0.0f;

	if (!d || !f)
		return 0;
	memset(d, 0, sizeof(*d));
	d->axis = GIZMO_AXIS_NONE;

	axis = gizmo_hit(f, mode, sx, sy);
	if (axis == GIZMO_AXIS_NONE)
		return 0;

	switch (mode) {
	case GIZMO_MODE_TRANSLATE:
		if (!axis_distance(f, axis, sx, sy, &value))
			return 0;
		break;
	case GIZMO_MODE_ROTATE:
		if (!screen_angle(f, sx, sy, &value))
			return 0;
		break;
	case GIZMO_MODE_SCALE:
		if (!screen_radius(f, sx, sy, &value))
			return 0;
		break;
	default:
		return 0;
	}

	d->active      = 1;
	d->mode        = mode;
	d->axis        = axis;
	d->start       = f->pivot;
	d->start_value = value;
	d->start_origin[0] = f->origin[0];
	d->start_origin[1] = f->origin[1];
	d->start_origin[2] = f->origin[2];
	return 1;
}

static int32_t drag_translate(const struct gizmo_drag *d,
			      const struct gizmo_frame *f,
			      const struct gizmo_snap *snap, float sx, float sy,
			      struct transform *out)
{
	float now;
	float moved;

	if (!axis_distance(f, d->axis, sx, sy, &now))
		return 0;
	*out = d->start;
	moved = now - d->start_value;
	/*
	 * The snapped quantity is the resulting coordinate, not the delta. A
	 * reader who turns snapping on wants the object on the grid, and
	 * snapping the delta would leave an object that started off-grid off it
	 * for ever.
	 */
	out->position[d->axis] = snap_to(d->start.position[d->axis] + moved,
					 snap ? snap->translate : 0.0f);
	return 1;
}

static int32_t drag_rotate(const struct gizmo_drag *d,
			   const struct gizmo_frame *f,
			   const struct gizmo_snap *snap, float sx, float sy,
			   struct transform *out)
{
	float now, turned, half, step;
	float axis_q[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

	if (!screen_angle(f, sx, sy, &now))
		return 0;

	turned = (now - d->start_value) * axis_facing(f, d->axis);
	/*
	 * Unwrapped into (-pi, pi]. atan2 jumps by 2pi as the pointer crosses
	 * the -x ray, and without this the object spins a full turn the instant
	 * it does.
	 */
	while (turned > GIZMO_PI)
		turned -= 2.0f * GIZMO_PI;
	while (turned < -GIZMO_PI)
		turned += 2.0f * GIZMO_PI;

	step = snap && snap->rotate_degrees > 0.0f
		       ? snap->rotate_degrees * GIZMO_PI / 180.0f
		       : 0.0f;
	turned = snap_to(turned, step);

	half             = turned * 0.5f;
	axis_q[d->axis]  = sinf(half);
	axis_q[3]        = cosf(half);

	*out = d->start;
	/*
	 * Left-multiplied: the handles are world axes, so the turn happens
	 * about the world axis regardless of how the entity is already
	 * oriented. Right-multiplying would make it a local-space turn, which
	 * is a different (and also useful) gizmo that this one is not.
	 */
	quat_mul(out->rotation, axis_q, d->start.rotation);
	return 1;
}

static int32_t drag_scale(const struct gizmo_drag *d,
			  const struct gizmo_frame *f,
			  const struct gizmo_snap *snap, float sx, float sy,
			  struct transform *out)
{
	float now, ratio;
	int   c;

	if (!screen_radius(f, sx, sy, &now))
		return 0;
	if (d->start_value <= 1.0f)
		return 0;

	ratio = now / d->start_value;
	/*
	 * Clamped away from zero rather than allowed through it. A scale of
	 * exactly 0 collapses the entity to a point it cannot be dragged back
	 * out of — the handles have no screen length once it is there — and a
	 * negative one mirrors it, which no reader has ever meant by dragging
	 * inward.
	 */
	if (ratio < 0.01f)
		ratio = 0.01f;

	*out = d->start;
	for (c = 0; c < 3; c++) {
		if (d->axis != GIZMO_AXIS_ALL && c != (int)d->axis)
			continue;
		out->scale[c] = snap_to(d->start.scale[c] * ratio,
					snap ? snap->scale : 0.0f);
		/* Snapping must not be able to produce the zero it forbids. */
		if (out->scale[c] <= 0.0f)
			out->scale[c] = snap && snap->scale > 0.0f
						? snap->scale
						: 0.01f;
	}
	return 1;
}

int32_t gizmo_drag_to(struct gizmo_drag *d, const struct gizmo_frame *f,
		      const struct gizmo_snap *snap, float sx, float sy,
		      struct transform *out)
{
	struct gizmo_frame live;

	if (!d || !f || !out || !d->active)
		return 0;
	if (d->axis == GIZMO_AXIS_NONE)
		return 0;

	/*
	 * The camera and the basis are this frame's; the pivot and the handle
	 * origin are the drag's. Taking the live ones would feed the last
	 * result back into the next solve, which integrates — and the header
	 * says why that is the wrong shape.
	 */
	live           = *f;
	live.pivot     = d->start;
	live.origin[0] = d->start_origin[0];
	live.origin[1] = d->start_origin[1];
	live.origin[2] = d->start_origin[2];

	switch (d->mode) {
	case GIZMO_MODE_TRANSLATE:
		return drag_translate(d, &live, snap, sx, sy, out);
	case GIZMO_MODE_ROTATE:
		return drag_rotate(d, &live, snap, sx, sy, out);
	case GIZMO_MODE_SCALE:
		return drag_scale(d, &live, snap, sx, sy, out);
	default:
		return 0;
	}
}

void gizmo_end(struct gizmo_drag *d)
{
	if (!d)
		return;
	memset(d, 0, sizeof(*d));
	d->axis = GIZMO_AXIS_NONE;
}
