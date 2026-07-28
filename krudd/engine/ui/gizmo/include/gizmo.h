/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef GIZMO_H
#define GIZMO_H

#include "math_types.h"

#include <stdint.h>

/*
 * gizmo — the transform handles, and the arithmetic behind them (#949).
 *
 * ## Why this is a C module and not a React component
 *
 * #944's Q4 decided it: kruddgui is the game's GUI, it draws in the canvas, and
 * the editor's gizmos are its second tenant. The alternative — an SVG overlay
 * projected from a view·projection the editor read across the boundary — is
 * always one frame stale and always slightly wrong against the depth buffer,
 * and it would put a second copy of the camera's arithmetic in TypeScript.
 *
 * ## Who drives it
 *
 * The editor. It owns the pointer over the canvas (that is #949's one-owner
 * rule) and it feeds this module screen positions through the bridge. This
 * module does not read kruddgui's pointer, and must not start to: two owners of
 * one pointer is exactly the failure the rule exists to prevent.
 *
 * So the drag is a three-call protocol — begin, move, end — and `begin` is the
 * one that answers a question: did the press land on a handle. The editor waits
 * one frame for that answer before deciding whether the gesture is a gizmo drag
 * or a camera orbit. One frame is the same latency #944's Q1 accepted for every
 * other read, and it is the frame the drag had not moved in anyway.
 *
 * ## What it does not do
 *
 * It does not touch the world. `gizmo_drag` computes a transform and hands it
 * back; the bridge applies it through entity_api like any other edit, so a
 * gizmo drag lands on `world/edit`'s ring under the editor's gesture and
 * coalesces by key exactly as an inspector slider does. That equivalence is
 * #944's Q2 payoff and it stays free only as long as nothing here mutates.
 */

/* Which handle set is live. Mirrors the editor's gizmo mode. */
enum gizmo_mode {
	GIZMO_MODE_NONE		= 0,
	GIZMO_MODE_TRANSLATE	= 1,
	GIZMO_MODE_ROTATE	= 2,
	GIZMO_MODE_SCALE	= 3
};

/* Which handle a drag grabbed. The axis handles are the world axes. */
enum gizmo_axis {
	GIZMO_AXIS_NONE	= -1,
	GIZMO_AXIS_X	= 0,
	GIZMO_AXIS_Y	= 1,
	GIZMO_AXIS_Z	= 2,
	/* Scale only: the centre handle, which scales all three at once. */
	GIZMO_AXIS_ALL	= 3
};

/*
 * How far from a handle a press still counts as grabbing it, in CSS pixels.
 *
 * Generous on purpose. A 6px line is a 6px target only for a mouse, and the
 * cost of an over-eager grab is one Escape; the cost of a miss is an orbit the
 * reader did not ask for, which moves the camera and loses their framing.
 */
#define GIZMO_GRAB_PIXELS	11.0f

/* The handle bar's length in pixels — see gizmo_screen_scale. */
#define GIZMO_ARM_PIXELS	72.0f

/*
 * How many points a rotation ring is walked in.
 *
 * The same number for drawing and for hit-testing, because it is the same
 * walk: #813's finding was that a hit-test must be generated through the path
 * that draws, and a ring tested as an ideal ellipse while drawn as 48 segments
 * is that divergence in miniature. 48 is smooth at any size a gizmo is usable
 * at, and it is 48 line calls a frame for the one selected entity.
 */
#define GIZMO_RING_STEPS	48

/*
 * How much of a rotation ring's radius, around the pivot, is not a handle.
 *
 * A ring seen edge-on projects to a line *through* the pivot, so without this
 * the middle of a rotate gizmo would always be grabbable — by whichever of the
 * three rings happens to be most edge-on, which is the one whose rotation is
 * least usable. Reserving the centre makes the mode read the way it looks:
 * grab a rim, not the hub.
 */
#define GIZMO_RING_DEAD_ZONE	0.35f

/*
 * Everything the arithmetic needs about the frame a drag happens in.
 *
 * Passed rather than resolved so every function below is pure: gizmo_test
 * drives the whole of the maths against a synthetic camera with no GPU, no
 * world and no subsystem manager, which is what the issue asks for when it says
 * to test the transform arithmetic directly rather than by eye.
 *
 * ## Why the pivot and the origin are two fields
 *
 * The handles are drawn where the entity *is* — world space, so they land on
 * the pixels the mesh was drawn to. The transform they edit is the entity's
 * *local* one, because that is what entity_api's set_transform takes and the
 * only one an edit can name.
 *
 * For an unparented entity those are the same numbers and the distinction costs
 * nothing. For a parented one they are not, and collapsing them is how a gizmo
 * ends up drawing an arrow along world X while dragging the object along its
 * parent's X. So: `origin` and `basis` describe where the handles go and which
 * way they point (the parent's frame, which for a root entity is the world's),
 * and `pivot` is the local transform the drag rewrites.
 */
struct gizmo_frame {
	/* The exact view·projection the forward pass draws with. */
	struct mat4	view_proj;
	/* Camera eye, world space. Sizes the handles by distance. */
	float		eye[3];
	/* Viewport size, CSS pixels. */
	float		width;
	float		height;

	/* Where the handles are drawn and hit-tested: the entity's world
	 * position. */
	float		origin[3];
	/*
	 * Unit world-space direction of each handle, x, y, z. The parent's
	 * world rotation applied to the three unit axes — identity for a root
	 * entity, which is the case that has to stay free.
	 */
	float		basis[3][3];
	/*
	 * World units per local unit along each handle. The parent's world
	 * scale. Divided out of the translate solve so dragging a handle one
	 * arm's length moves the entity one arm's length on screen whatever
	 * its parent is scaled by.
	 */
	float		axis_scale[3];

	/* The local transform the drag rewrites. */
	struct transform pivot;
};

/*
 * The snap increments. Zero on any field means that mode does not snap.
 *
 * Rotation is in degrees because that is the number a reader types; the
 * arithmetic converts once, here, rather than at every call site.
 */
struct gizmo_snap {
	float	translate;
	float	rotate_degrees;
	float	scale;
};

/* A drag in progress. Opaque to callers except through the functions below. */
struct gizmo_drag {
	int32_t			active;
	enum gizmo_mode		mode;
	enum gizmo_axis		axis;
	struct transform	start;		/* the pivot when the drag began */
	/*
	 * Where the handles were when the drag began, world space.
	 *
	 * Frozen rather than re-read, because the entity moves under the drag:
	 * a solve against handles that chase the object would feed the last
	 * answer into the next one, and a translate would run away from the
	 * pointer at the rate it was being dragged.
	 */
	float			start_origin[3];
	/*
	 * The scalar the press mapped to, in the drag's own units: distance
	 * along the axis for translate, radians for rotate, a screen radius for
	 * scale. The delta applied is always (now - start), so a drag is
	 * absolute against its own origin and a snapped drag does not
	 * accumulate rounding.
	 */
	float			start_value;
};

/*
 * Project a world point to viewport pixels (top-left origin).
 *
 * Returns 1 and writes out[2] when the point is in front of the camera, 0 when
 * it is behind it or the matrix is degenerate — a handle behind the eye
 * projects to a mirrored position that would otherwise be drawn and, worse,
 * hit-tested.
 */
int32_t gizmo_project(const struct gizmo_frame *f, const float world[3],
		      float out[2]);

/*
 * World units per pixel at the origin, so the handles are a constant size on
 * screen however far away the selection is. Returns 0 when the frame is
 * degenerate, in which case nothing should be drawn.
 */
float gizmo_screen_scale(const struct gizmo_frame *f);

/*
 * Where the three axis handle tips sit in world space, given the origin, the
 * basis and the screen scale. Written into out[3][3] in x, y, z order.
 */
void gizmo_handle_tips(const struct gizmo_frame *f, float out[3][3]);

/*
 * Fill in `basis` as the identity and `axis_scale` as ones — the frame of a
 * root entity, and the starting point every caller wants before it overwrites
 * them for a parented one. Leaves every other field alone.
 */
void gizmo_frame_identity_basis(struct gizmo_frame *f);

/*
 * One point on the rotation ring for `axis`, in world space.
 *
 * `step` runs 0..GIZMO_RING_STEPS inclusive, so a caller that walks the whole
 * range closes the loop. The ring lies in the plane **perpendicular** to its
 * axis — that is the circle a turn about that axis actually traces, and getting
 * it the other way round gives a ring that vanishes exactly when its rotation
 * is most usable (the axis pointing at the camera).
 *
 * Its radius is the handle arm's, so the rings and the arms are the same size
 * and switching modes does not move the target under the reader's pointer.
 */
void gizmo_ring_point(const struct gizmo_frame *f, enum gizmo_axis axis,
		      int32_t step, float out[3]);

/*
 * Hit-test a press. Returns the handle it grabbed, or GIZMO_AXIS_NONE.
 *
 * Translate and scale test the distance to the handle's screen segment; rotate
 * tests the distance to the handle's screen circle, because a rotation ring is
 * grabbed on its rim rather than along a bar. Ties go to the nearest.
 */
enum gizmo_axis gizmo_hit(const struct gizmo_frame *f, enum gizmo_mode mode,
			  float sx, float sy);

/*
 * Open a drag on whatever (sx, sy) grabbed. Returns 1 when a handle was
 * grabbed and *d is now active, 0 when the press missed and *d is left inert.
 */
int32_t gizmo_begin(struct gizmo_drag *d, const struct gizmo_frame *f,
		    enum gizmo_mode mode, float sx, float sy);

/*
 * Advance an open drag to (sx, sy) and write the transform the selection should
 * now have into *out. Returns 1 when *out was written, 0 when the drag is not
 * active or the pointer maps to nothing usable this frame (a pointer exactly on
 * the pivot during a rotate has no angle, and holding the last transform is a
 * better answer than snapping to an arbitrary one).
 *
 * `f` carries the *live* frame, so a drag survives the camera moving under it.
 * The pivot in it is ignored — the drag's own start pivot is the origin, which
 * is what keeps a snapped drag from accumulating rounding as it goes.
 */
int32_t gizmo_drag_to(struct gizmo_drag *d, const struct gizmo_frame *f,
		      const struct gizmo_snap *snap, float sx, float sy,
		      struct transform *out);

/* Close a drag. Safe on one that is not open. */
void gizmo_end(struct gizmo_drag *d);

/*
 * The "gizmo" subsystem's vtable, published for the bridge (#949).
 *
 * Mode and snap are engine state rather than editor state for the same reason
 * the selection is: two things draw from them — the overlay here and the
 * editor's own mode buttons — and a second copy is a second thing to
 * disagree.
 */
struct gizmo_api {
	void		(*set_mode)(int32_t mode);
	int32_t		(*get_mode)(void);
	void		(*set_snap)(float translate, float rotate_degrees,
				    float scale);
	void		(*get_snap)(struct gizmo_snap *out);

	/*
	 * Begin / move / end, in viewport pixels. begin returns the grabbed
	 * axis (or GIZMO_AXIS_NONE); move returns 1 when it wrote *out.
	 */
	int32_t		(*begin)(float sx, float sy);
	int32_t		(*move)(float sx, float sy, struct transform *out);
	void		(*end)(void);

	/* Which axis a live drag holds, or GIZMO_AXIS_NONE when none is. */
	int32_t		(*axis)(void);

	/* Whether the grid draws, and how far apart its lines are. */
	void		(*set_grid)(int32_t shown, float spacing);
	int32_t		(*grid_shown)(void);
	float		(*grid_spacing)(void);
};

#endif /* GIZMO_H */
