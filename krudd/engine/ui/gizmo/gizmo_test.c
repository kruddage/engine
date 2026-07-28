/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * gizmo — the transform arithmetic, GPU-free.
 *
 * #949 asks for this in as many words: "gizmo maths is easy to get subtly wrong
 * and miserable to debug by eye. Test the transform arithmetic directly rather
 * than only through the UI." So every assertion here is on a number, and none
 * of them needs a canvas, a world or a subsystem manager — a real
 * view·projection built from the same camera the renderer uses, a pivot, a
 * pointer position, and the transform that comes out.
 *
 * The camera looks down +Z at the origin throughout, so world +X is screen
 * right and world +Y is screen up. That framing is the one thing worth holding
 * in your head while reading the expectations below.
 */
#include "gizmo.h"

#include "camera.h"
#include "math_types.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define VW 800.0f
#define VH 600.0f

static int g_passed;

#define CHECK(name, expr)						\
	do {								\
		if (expr) {						\
			printf("PASS: %s\n", (name));			\
			g_passed++;					\
		} else {						\
			printf("FAIL: %s\n", (name));			\
			assert(0 && (name));				\
		}							\
	} while (0)

static int near(float a, float b, float tol)
{
	return fabsf(a - b) <= tol;
}

/* A frame with the camera at +Z looking at the origin and the pivot at `at`. */
static void frame_at(struct gizmo_frame *f, float x, float y, float z)
{
	struct camera cam;

	memset(f, 0, sizeof(*f));
	memset(&cam, 0, sizeof(cam));
	cam.eye[2]    = 8.0f;
	cam.up[1]     = 1.0f;
	cam.fov_y     = 0.8f;
	cam.aspect    = VW / VH;
	cam.near      = 0.1f;
	cam.far       = 100.0f;
	camera_update(&cam);

	f->view_proj = cam.view_proj;
	f->eye[0]    = cam.eye[0];
	f->eye[1]    = cam.eye[1];
	f->eye[2]    = cam.eye[2];
	f->width     = VW;
	f->height    = VH;

	f->origin[0] = x;
	f->origin[1] = y;
	f->origin[2] = z;
	gizmo_frame_identity_basis(f);

	f->pivot.position[0] = x;
	f->pivot.position[1] = y;
	f->pivot.position[2] = z;
	f->pivot.rotation[3] = 1.0f;
	f->pivot.scale[0] = f->pivot.scale[1] = f->pivot.scale[2] = 1.0f;
}

/* The pivot's own screen position — the point every handle radiates from. */
static void pivot_pixel(const struct gizmo_frame *f, float out[2])
{
	assert(gizmo_project(f, f->origin, out));
}

/* ------------------------------------------------------------------ *
 * Projection and sizing
 * ------------------------------------------------------------------ */

static void test_projection(void)
{
	struct gizmo_frame f;
	float              p[2];
	float              behind[3] = { 0.0f, 0.0f, 20.0f };

	frame_at(&f, 0.0f, 0.0f, 0.0f);

	pivot_pixel(&f, p);
	CHECK("origin projects to the centre pixel",
	      near(p[0], VW * 0.5f, 0.5f) && near(p[1], VH * 0.5f, 0.5f));

	/* +Y is up in the world and *down* the screen in pixels. */
	{
		float up[3] = { 0.0f, 1.0f, 0.0f };

		assert(gizmo_project(&f, up, p));
		CHECK("world +Y projects above the centre", p[1] < VH * 0.5f);
	}
	{
		float right[3] = { 1.0f, 0.0f, 0.0f };

		assert(gizmo_project(&f, right, p));
		CHECK("world +X projects right of the centre", p[0] > VW * 0.5f);
	}

	CHECK("a point behind the eye does not project",
	      gizmo_project(&f, behind, p) == 0);

	f.width = 0.0f;
	CHECK("a zero-width viewport does not project",
	      gizmo_project(&f, f.origin, p) == 0);
}

static void test_handles_hold_their_screen_size(void)
{
	struct gizmo_frame near_f, far_f;
	float              tips[3][3];
	float              centre[2], tip[2];
	float              near_len, far_len;

	frame_at(&near_f, 0.0f, 0.0f, 0.0f);
	far_f = near_f;
	/* Same camera, a pivot four times further away. */
	far_f.origin[2] = -24.0f;
	far_f.pivot.position[2] = -24.0f;

	gizmo_handle_tips(&near_f, tips);
	pivot_pixel(&near_f, centre);
	assert(gizmo_project(&near_f, tips[0], tip));
	near_len = fabsf(tip[0] - centre[0]);

	gizmo_handle_tips(&far_f, tips);
	pivot_pixel(&far_f, centre);
	assert(gizmo_project(&far_f, tips[0], tip));
	far_len = fabsf(tip[0] - centre[0]);

	/*
	 * The whole point of scaling by eye distance: an arm is the same
	 * number of pixels whether the selection is near or far. Loose
	 * tolerance because the projection is perspective and the arm has
	 * length, so the two are not identical — but they are within a few
	 * percent, not within a factor of four.
	 */
	CHECK("a handle is the same pixel length near and far",
	      near_len > 10.0f && near(near_len, far_len, near_len * 0.1f));

	CHECK("a degenerate frame has no screen scale",
	      gizmo_screen_scale(NULL) == 0.0f);
}

/* ------------------------------------------------------------------ *
 * Hit-testing
 * ------------------------------------------------------------------ */

static void test_hit(void)
{
	struct gizmo_frame f;
	float              tips[3][3];
	float              centre[2], tip[2];

	frame_at(&f, 0.0f, 0.0f, 0.0f);
	pivot_pixel(&f, centre);
	gizmo_handle_tips(&f, tips);

	assert(gizmo_project(&f, tips[GIZMO_AXIS_X], tip));
	CHECK("a press on the X arm grabs X",
	      gizmo_hit(&f, GIZMO_MODE_TRANSLATE, tip[0], tip[1]) ==
		      GIZMO_AXIS_X);

	assert(gizmo_project(&f, tips[GIZMO_AXIS_Y], tip));
	CHECK("a press on the Y arm grabs Y",
	      gizmo_hit(&f, GIZMO_MODE_TRANSLATE, tip[0], tip[1]) ==
		      GIZMO_AXIS_Y);

	CHECK("a press well clear of every handle grabs nothing",
	      gizmo_hit(&f, GIZMO_MODE_TRANSLATE, centre[0] + 300.0f,
			centre[1] + 220.0f) == GIZMO_AXIS_NONE);

	CHECK("no mode means no handles",
	      gizmo_hit(&f, GIZMO_MODE_NONE, centre[0], centre[1]) ==
		      GIZMO_AXIS_NONE);

	/*
	 * The centre handle is scale-only, and it has to win over the three
	 * arms that all start there — see the note in gizmo_hit.
	 */
	CHECK("the centre grabs uniform scale in scale mode",
	      gizmo_hit(&f, GIZMO_MODE_SCALE, centre[0], centre[1]) ==
		      GIZMO_AXIS_ALL);
	CHECK("the centre grabs no uniform handle in translate mode",
	      gizmo_hit(&f, GIZMO_MODE_TRANSLATE, centre[0], centre[1]) !=
		      GIZMO_AXIS_ALL);

	/*
	 * Rotate grabs a ring on its rim. A press at the pivot is inside every
	 * ring and on none of them, so it must miss — otherwise clicking the
	 * middle of the gizmo would start an arbitrary rotation.
	 */
	CHECK("a press at the centre grabs no rotation ring",
	      gizmo_hit(&f, GIZMO_MODE_ROTATE, centre[0], centre[1]) ==
		      GIZMO_AXIS_NONE);

	/*
	 * The Z ring is the one facing this camera — it lies in the XY plane,
	 * which is the screen plane — so its rim is a full circle at the arm's
	 * radius, screen-right of the centre.
	 *
	 * This is the case that was wrong first: a ring drawn *along* its axis
	 * rather than perpendicular to it collapses to a point for exactly the
	 * axis whose rotation is most usable, and then swallows every press at
	 * the centre.
	 */
	{
		float ring[3];

		gizmo_ring_point(&f, GIZMO_AXIS_Z, 0, ring);
		assert(gizmo_project(&f, ring, tip));
		CHECK("the camera-facing ring has a real screen radius",
		      tip[0] - centre[0] > 20.0f);

		/*
		 * Tested an eighth of the way round rather than at step 0.
		 *
		 * Seen exactly down an axis the other two rings are edge-on,
		 * so they project to a horizontal and a vertical line through
		 * the pivot — and the ends of those lines land precisely on
		 * the face-on ring's rim. A press there is genuinely on two
		 * rings at once and either answer is defensible; a press
		 * between the axes is on one, and that is what is worth
		 * pinning.
		 */
		gizmo_ring_point(&f, GIZMO_AXIS_Z, GIZMO_RING_STEPS / 8, ring);
		assert(gizmo_project(&f, ring, tip));
		CHECK("a press on the Z ring's rim grabs Z",
		      gizmo_hit(&f, GIZMO_MODE_ROTATE, tip[0], tip[1]) ==
			      GIZMO_AXIS_Z);

		/*
		 * The X ring lies in the YZ plane, so from here it is the
		 * vertical line through the pivot. A point partway up it is
		 * inside the Z ring and clear of the dead zone, so it belongs
		 * to X alone.
		 */
		gizmo_ring_point(&f, GIZMO_AXIS_X, GIZMO_RING_STEPS / 6, ring);
		assert(gizmo_project(&f, ring, tip));
		CHECK("a press on the edge-on X ring grabs X",
		      gizmo_hit(&f, GIZMO_MODE_ROTATE, tip[0], tip[1]) ==
			      GIZMO_AXIS_X);
	}
}

/* ------------------------------------------------------------------ *
 * Translate
 * ------------------------------------------------------------------ */

static void test_translate(void)
{
	struct gizmo_frame f;
	struct gizmo_drag  d;
	struct transform   out;
	struct gizmo_snap  none = { 0.0f, 0.0f, 0.0f };
	struct gizmo_snap  half = { 0.5f, 0.0f, 0.0f };
	float              tips[3][3];
	float              centre[2], tip[2];
	float              arm_pixels;

	frame_at(&f, 0.0f, 0.0f, 0.0f);
	pivot_pixel(&f, centre);
	gizmo_handle_tips(&f, tips);
	assert(gizmo_project(&f, tips[GIZMO_AXIS_X], tip));
	arm_pixels = tip[0] - centre[0];

	CHECK("a drag opens on the arm it was pressed on",
	      gizmo_begin(&d, &f, GIZMO_MODE_TRANSLATE, tip[0], tip[1]) &&
		      d.axis == GIZMO_AXIS_X);

	/*
	 * Dragging to exactly where the arm ends should move the entity by
	 * exactly one arm's length in world units — that is the whole content
	 * of the axis_distance solve, and the number it must produce is
	 * GIZMO_ARM_PIXELS worth of world.
	 */
	assert(gizmo_drag_to(&d, &f, &none, tip[0] + arm_pixels, tip[1], &out));
	CHECK("dragging one arm's length moves one arm's length",
	      near(out.position[0], GIZMO_ARM_PIXELS * gizmo_screen_scale(&f),
		   0.02f));
	CHECK("an axis drag moves nothing off its axis",
	      out.position[1] == 0.0f && out.position[2] == 0.0f);
	CHECK("an axis drag leaves rotation and scale alone",
	      out.rotation[3] == 1.0f && out.scale[0] == 1.0f);

	/* Back to where the press started: exactly the original position. */
	assert(gizmo_drag_to(&d, &f, &none, tip[0], tip[1], &out));
	CHECK("returning the pointer returns the entity",
	      near(out.position[0], 0.0f, 1e-4f));

	/*
	 * Absolute, not integrated: a jump straight to the far pointer gives
	 * the same answer as walking there, because the solve is against the
	 * drag's own origin. A drift here is the bug the design note warns of.
	 */
	{
		struct gizmo_drag walked;
		struct transform  step;
		int               i;

		assert(gizmo_begin(&walked, &f, GIZMO_MODE_TRANSLATE, tip[0],
				   tip[1]));
		for (i = 1; i <= 10; i++)
			assert(gizmo_drag_to(&walked, &f, &none,
					     tip[0] + arm_pixels * (float)i /
							     10.0f,
					     tip[1], &step));
		assert(gizmo_drag_to(&d, &f, &none, tip[0] + arm_pixels, tip[1],
				     &out));
		CHECK("a walked drag lands where a jumped one does",
		      near(step.position[0], out.position[0], 1e-4f));
	}

	/* Snapping quantizes the resulting coordinate, not the delta. */
	{
		float snapped;

		assert(gizmo_drag_to(&d, &f, &half, tip[0] + arm_pixels * 0.55f,
				     tip[1], &out));
		snapped = out.position[0];
		CHECK("a snapped translate lands on a multiple of the step",
		      near(fmodf(fabsf(snapped) + 1e-4f, 0.5f), 0.0f, 2e-3f));
	}

	/*
	 * An entity that starts off-grid snaps onto the grid rather than
	 * keeping its offset — which is what snapping the delta would do, and
	 * is the reason the note in drag_translate exists.
	 */
	{
		struct gizmo_frame off;
		struct gizmo_drag  od;

		frame_at(&off, 0.0f, 0.0f, 0.0f);
		off.pivot.position[0] = 0.13f;
		pivot_pixel(&off, centre);
		gizmo_handle_tips(&off, tips);
		assert(gizmo_project(&off, tips[GIZMO_AXIS_X], tip));
		assert(gizmo_begin(&od, &off, GIZMO_MODE_TRANSLATE, tip[0],
				   tip[1]));
		assert(gizmo_drag_to(&od, &off, &half, tip[0] + arm_pixels,
				     tip[1], &out));
		CHECK("snapping pulls an off-grid entity onto the grid",
		      near(fmodf(fabsf(out.position[0]) + 1e-4f, 0.5f), 0.0f,
			   2e-3f));
	}

	gizmo_end(&d);
	CHECK("an ended drag holds no axis", d.axis == GIZMO_AXIS_NONE);
	CHECK("an ended drag writes nothing",
	      gizmo_drag_to(&d, &f, &none, tip[0], tip[1], &out) == 0);
}

/* ------------------------------------------------------------------ *
 * Rotate
 * ------------------------------------------------------------------ */

/* The angle of the rotation `q` about the axis it turns, in radians. */
static float quat_angle(const float q[4])
{
	float w = q[3];

	if (w > 1.0f)
		w = 1.0f;
	else if (w < -1.0f)
		w = -1.0f;
	return 2.0f * acosf(fabsf(w));
}

static void test_rotate(void)
{
	struct gizmo_frame f;
	struct gizmo_drag  d;
	struct transform   out;
	struct gizmo_snap  none = { 0.0f, 0.0f, 0.0f };
	struct gizmo_snap  deg15 = { 0.0f, 15.0f, 0.0f };
	float              tips[3][3];
	float              centre[2], tip[2], radius;

	frame_at(&f, 0.0f, 0.0f, 0.0f);
	pivot_pixel(&f, centre);
	gizmo_handle_tips(&f, tips);
	/*
	 * The Z ring faces this camera, so it is the usable one. Grabbed an
	 * eighth of the way round rather than at step 0, where the two edge-on
	 * rings also pass — see the note in test_hit.
	 */
	{
		float ring[3];

		gizmo_ring_point(&f, GIZMO_AXIS_Z, 0, ring);
		assert(gizmo_project(&f, ring, tip));
		radius = tip[0] - centre[0];

		gizmo_ring_point(&f, GIZMO_AXIS_Z, GIZMO_RING_STEPS / 8, ring);
		assert(gizmo_project(&f, ring, tip));
	}

	assert(gizmo_begin(&d, &f, GIZMO_MODE_ROTATE, tip[0], tip[1]));
	CHECK("a rotate drag opens on the Z ring", d.axis == GIZMO_AXIS_Z);

	/*
	 * A quarter turn around the ring is a quarter turn of the entity. The
	 * pointer starts at screen angle -45 (up and to the right, because
	 * screen y runs down) and moves to +45, which is a quarter turn.
	 */
	assert(gizmo_drag_to(&d, &f, &none,
			     centre[0] + radius * 0.7071f,
			     centre[1] + radius * 0.7071f, &out));
	CHECK("a quarter turn on the ring is a quarter turn of the entity",
	      near(quat_angle(out.rotation), 1.5707963f, 0.02f));
	CHECK("a Z rotation only touches the Z component",
	      near(out.rotation[0], 0.0f, 1e-5f) &&
		      near(out.rotation[1], 0.0f, 1e-5f));
	CHECK("a rotate drag leaves position and scale alone",
	      out.position[0] == 0.0f && out.scale[0] == 1.0f);

	/* Back to the start angle: back to no rotation at all. */
	assert(gizmo_drag_to(&d, &f, &none, tip[0], tip[1], &out));
	CHECK("returning the pointer returns the rotation",
	      near(quat_angle(out.rotation), 0.0f, 1e-3f));

	/*
	 * Unwrapping. atan2 jumps by 2pi across the -x ray; a drag that walked
	 * across it without unwrapping would spin a full turn in one frame.
	 * Just past the far side must read as a bit under half a turn, not as
	 * a bit over three-quarters of one.
	 */
	assert(gizmo_drag_to(&d, &f, &none, centre[0] - radius,
			     centre[1] - radius * 0.05f, &out));
	CHECK("a drag past the -x ray does not spin a full turn",
	      quat_angle(out.rotation) < 3.2f);

	/* Snapping: 15 degrees, so every reachable angle is a multiple of it. */
	{
		float degrees;

		assert(gizmo_drag_to(&d, &f, &deg15, centre[0] + radius * 0.7f,
				     centre[1] + radius * 0.7f, &out));
		degrees = quat_angle(out.rotation) * 180.0f / 3.14159265f;
		CHECK("a snapped rotation lands on a multiple of the step",
		      near(fmodf(degrees + 0.05f, 15.0f), 0.0f, 0.2f));
	}

	/*
	 * A pointer sitting exactly on the pivot has no angle. Holding the
	 * last transform is the right answer; picking an arbitrary one is how
	 * an object flips when a drag passes through the centre.
	 */
	CHECK("a pointer on the pivot writes no rotation",
	      gizmo_drag_to(&d, &f, &none, centre[0], centre[1], &out) == 0);
	gizmo_end(&d);
}

/* ------------------------------------------------------------------ *
 * Scale
 * ------------------------------------------------------------------ */

static void test_scale(void)
{
	struct gizmo_frame f;
	struct gizmo_drag  d;
	struct transform   out;
	struct gizmo_snap  none = { 0.0f, 0.0f, 0.0f };
	struct gizmo_snap  quarter = { 0.0f, 0.0f, 0.25f };
	float              tips[3][3];
	float              centre[2], tip[2], radius;

	frame_at(&f, 0.0f, 0.0f, 0.0f);
	pivot_pixel(&f, centre);
	gizmo_handle_tips(&f, tips);
	assert(gizmo_project(&f, tips[GIZMO_AXIS_X], tip));
	radius = tip[0] - centre[0];

	assert(gizmo_begin(&d, &f, GIZMO_MODE_SCALE, tip[0], tip[1]));
	CHECK("a scale drag opens on the X handle", d.axis == GIZMO_AXIS_X);

	/* Twice as far from the pivot is twice the scale. */
	assert(gizmo_drag_to(&d, &f, &none, centre[0] + radius * 2.0f, tip[1],
			     &out));
	CHECK("dragging out to twice the radius doubles the scale",
	      near(out.scale[0], 2.0f, 0.01f));
	CHECK("an axis scale leaves the other axes alone",
	      out.scale[1] == 1.0f && out.scale[2] == 1.0f);
	CHECK("a scale drag leaves position and rotation alone",
	      out.position[0] == 0.0f && out.rotation[3] == 1.0f);

	/*
	 * Dragging all the way in must not reach zero. A zero-scaled entity has
	 * no handles with any screen length, so it is one a reader cannot drag
	 * back out — which makes it a trap rather than an edit.
	 */
	assert(gizmo_drag_to(&d, &f, &none, centre[0] + 1.5f, centre[1], &out));
	CHECK("scaling all the way in never reaches zero", out.scale[0] > 0.0f);
	gizmo_end(&d);

	/* Uniform: the centre handle moves all three together. */
	assert(gizmo_begin(&d, &f, GIZMO_MODE_SCALE, tip[0], tip[1]));
	d.axis = GIZMO_AXIS_ALL;
	assert(gizmo_drag_to(&d, &f, &none, centre[0] + radius * 2.0f, tip[1],
			     &out));
	CHECK("the uniform handle scales all three axes",
	      near(out.scale[0], 2.0f, 0.01f) &&
		      near(out.scale[1], out.scale[0], 1e-5f) &&
		      near(out.scale[2], out.scale[0], 1e-5f));
	gizmo_end(&d);

	/* Snapped, and never snapped down to the zero the clamp forbids. */
	assert(gizmo_begin(&d, &f, GIZMO_MODE_SCALE, tip[0], tip[1]));
	assert(gizmo_drag_to(&d, &f, &quarter, centre[0] + radius * 2.1f,
			     tip[1], &out));
	CHECK("a snapped scale lands on a multiple of the step",
	      near(fmodf(out.scale[0] + 1e-4f, 0.25f), 0.0f, 2e-3f));
	assert(gizmo_drag_to(&d, &f, &quarter, centre[0] + 1.5f, centre[1],
			     &out));
	CHECK("a snapped scale never lands on zero", out.scale[0] > 0.0f);
	gizmo_end(&d);
}

/* ------------------------------------------------------------------ *
 * A parented selection
 * ------------------------------------------------------------------ */

/*
 * The case the two-field frame exists for. The entity sits at world (2, 0, 0)
 * because its parent is translated there and rotated a quarter turn about Y —
 * so the handle that moves the entity's *local* X points along world -Z.
 *
 * A gizmo that collapsed origin and pivot would draw the arrow along world X
 * and move the object along world -Z, which is the failure mode the header
 * describes.
 */
static void test_parented(void)
{
	struct gizmo_frame f;
	struct gizmo_drag  d;
	struct transform   out;
	struct gizmo_snap  none = { 0.0f, 0.0f, 0.0f };
	float              tips[3][3];
	float              centre[2], tip[2];

	frame_at(&f, 2.0f, 0.0f, 0.0f);
	/* Local position is the child's own, not the world one. */
	f.pivot.position[0] = 0.0f;

	/* A quarter turn about Y: local +X becomes world -Z. */
	f.basis[0][0] = 0.0f; f.basis[0][1] = 0.0f; f.basis[0][2] = -1.0f;
	f.basis[1][0] = 0.0f; f.basis[1][1] = 1.0f; f.basis[1][2] = 0.0f;
	f.basis[2][0] = 1.0f; f.basis[2][1] = 0.0f; f.basis[2][2] = 0.0f;

	gizmo_handle_tips(&f, tips);
	CHECK("a rotated parent points the X handle along its own axis",
	      near(tips[0][0], 2.0f, 1e-4f) && tips[0][2] < -0.01f);

	pivot_pixel(&f, centre);
	assert(gizmo_project(&f, tips[GIZMO_AXIS_X], tip));
	assert(gizmo_begin(&d, &f, GIZMO_MODE_TRANSLATE, tip[0], tip[1]));
	assert(gizmo_drag_to(&d, &f, &none, tip[0] + (tip[0] - centre[0]),
			     tip[1] + (tip[1] - centre[1]), &out));
	CHECK("a parented drag writes the local X, not the world one",
	      out.position[0] > 0.01f && out.position[2] == 0.0f);

	/*
	 * A parent scaled by two means one arm on screen is half an arm of
	 * local motion. Without dividing axis_scale out, dragging a parented
	 * child would move it twice as far as the pointer went.
	 */
	{
		struct gizmo_frame scaled;
		struct gizmo_drag  sd;
		struct transform   plain, halved;
		float              stips[3][3], sc[2], st[2];

		frame_at(&scaled, 0.0f, 0.0f, 0.0f);
		pivot_pixel(&scaled, sc);
		gizmo_handle_tips(&scaled, stips);
		assert(gizmo_project(&scaled, stips[GIZMO_AXIS_X], st));
		assert(gizmo_begin(&sd, &scaled, GIZMO_MODE_TRANSLATE, st[0],
				   st[1]));
		assert(gizmo_drag_to(&sd, &scaled, &none,
				     st[0] + (st[0] - sc[0]), st[1], &plain));

		scaled.axis_scale[0] = 2.0f;
		assert(gizmo_begin(&sd, &scaled, GIZMO_MODE_TRANSLATE, st[0],
				   st[1]));
		assert(gizmo_drag_to(&sd, &scaled, &none,
				     st[0] + (st[0] - sc[0]), st[1], &halved));
		CHECK("a parent scaled by two halves the local motion",
		      near(halved.position[0], plain.position[0] * 0.5f,
			   1e-3f));
	}
}

/* ------------------------------------------------------------------ *
 * Guards
 * ------------------------------------------------------------------ */

static void test_guards(void)
{
	struct gizmo_frame f;
	struct gizmo_drag  d;
	struct transform   out;
	struct gizmo_snap  none = { 0.0f, 0.0f, 0.0f };
	float              centre[2];

	frame_at(&f, 0.0f, 0.0f, 0.0f);
	pivot_pixel(&f, centre);

	CHECK("a press that grabs nothing opens no drag",
	      gizmo_begin(&d, &f, GIZMO_MODE_TRANSLATE, centre[0] + 400.0f,
			  centre[1]) == 0);
	CHECK("a drag that never opened writes nothing",
	      gizmo_drag_to(&d, &f, &none, centre[0], centre[1], &out) == 0);

	CHECK("NULL arguments never open a drag",
	      gizmo_begin(NULL, &f, GIZMO_MODE_TRANSLATE, 0.0f, 0.0f) == 0 &&
		      gizmo_begin(&d, NULL, GIZMO_MODE_TRANSLATE, 0.0f,
				  0.0f) == 0);

	/* Ending a drag that never began is a no-op, not a crash. */
	gizmo_end(&d);
	gizmo_end(NULL);
	CHECK("ending an unopened drag is safe", d.active == 0);
}

int main(void)
{
	test_projection();
	test_handles_hold_their_screen_size();
	test_hit();
	test_translate();
	test_rotate();
	test_scale();
	test_parented();
	test_guards();

	printf("\n%d/%d gizmo tests passed\n", g_passed, g_passed);
	return 0;
}
