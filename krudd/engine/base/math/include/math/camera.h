/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CAMERA_H
#define CAMERA_H

#include <math/math_types.h>

/*
 * One world unit is one metre. This is the engine's unit convention, stated
 * here because this is the header a WebXR view has to be built against:
 * WebXR reports head and controller poses in metres, in a space whose floor
 * is y = 0 (the "local-floor" reference space) — it does not offer a
 * choice. A renderer that draws a scene in world units and a runtime that
 * walks a real headset through it agree only if a unit and a metre are the
 * same thing; that agreement is what lets a headset pose be consumed
 * unscaled, with eye[3] set directly from it, rather than run through a
 * conversion this camera would otherwise have to carry.
 *
 * What the built-in meshes measure, so "sized in metres" is something a
 * caller can act on (world/asset/include/asset/builtin_mesh_scripts.h):
 *
 *   plane     a unit quad on XZ (x,z in ±0.5), facing +Y
 *   box       the unit cube, 1x1x1, centred on the origin
 *   cylinder  radius 0.5, height 1 — a unit across and a unit tall
 *   sphere    radius 0.5 — a unit across
 *   capsule   radius 0.5, cylinder length 1, TOTAL HEIGHT 2 — y in ±1
 *
 * Four of the five are one unit in every axis, so an entity's (scale ...)
 * IS its size in metres and nothing has to be remembered. capsule is the
 * exception: it stands two units tall at scale 1, so a 1.75 m figure needs
 * Y scale 0.875, not 1.75 — the one built-in where "the mesh's natural
 * size" is wrong by a factor of two, and so the one most worth getting
 * right before it is someone's height in a headset.
 *
 * The engine does not enforce this in code — nothing stops a project from
 * ignoring it — and chess does: a chess square is "one unit" with no metre
 * meaning, and does not need one, because nobody stands inside a chess
 * board. The convention matters for scenes meant to be occupied —
 * projects/training/training.scm is the first, and every WebXR scene after
 * it — not as a claim about every scene that already exists.
 */

/*
 * A camera is a (view, proj) pair and the world-space point view was built
 * about.  view_proj is their product, and that pair — not eye/target/fov — is
 * what the renderer draws with.
 *
 * eye/target/up + fov_y/aspect/near/far are ONE producer of that pair: the
 * authored look-at with a symmetric perspective, which camera_update() derives.
 * A caller that already holds a view and a projection (a rigid head pose and an
 * off-axis frustum encoding an interpupillary offset, say) sets them with
 * camera_set_view_proj() instead; those are consumed verbatim, and
 * camera_update() then leaves the camera alone until camera_clear_view_proj()
 * hands it back to the authored parameters.
 *
 * proj is in the GL convention (NDC z in [-1, 1]), the one mat4_perspective
 * produces — a supplied projection must be handed in that way, because it is
 * what mat4_clip_z01 adapts from and what CPU-side picking unprojects against.
 */
struct camera {
	/* The authored producer.  Inputs to camera_update(); nothing reads
	 * them once a pair has been supplied. */
	float       eye[3];
	float       target[3];
	float       up[3];
	float       fov_y;    /* vertical field of view, radians */
	float       aspect;   /* viewport width / height */
	float       near;
	float       far;

	/* The pair the renderer draws with, and the eye that goes with it. */
	struct mat4 view;
	struct mat4 proj;
	struct mat4 view_proj;   /* proj * view */
	float       position[3]; /* world eye of view; a shader's cam_pos */
	int         supplied;    /* nonzero while view/proj were set directly */
};

/*
 * Derive view, proj, view_proj and position from the camera's authored
 * parameters.  A no-op on a camera holding a supplied pair, so a per-frame
 * update never silently overwrites one.
 */
void camera_update(struct camera *cam);

/*
 * Draw with exactly this view and projection.  eye is the world-space position
 * view was built about — a supplied view matrix no longer implies cam->eye, and
 * shaders still need a camera position — so it is carried explicitly rather
 * than inferred.  Pins the pair against camera_update().
 */
void camera_set_view_proj(struct camera *cam, const struct mat4 *view,
			  const struct mat4 *proj, const float eye[3]);

/*
 * Hand the camera back to its authored parameters.  The pair survives until the
 * next camera_update(), which then rebuilds it from eye/target/up + fov.
 */
void camera_clear_view_proj(struct camera *cam);

#endif /* CAMERA_H */
