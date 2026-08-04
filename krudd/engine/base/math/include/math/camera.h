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
 * Fixed camera that produces a view_proj matrix each frame.
 * Interactive control is out of scope for v1; callers set eye/target/up
 * directly.  Call camera_update() after any change to recompute view_proj.
 */
struct camera {
	float       eye[3];
	float       target[3];
	float       up[3];
	float       fov_y;    /* vertical field of view, radians */
	float       aspect;   /* viewport width / height */
	float       near;
	float       far;
	struct mat4 view_proj; /* updated by camera_update() */
};

/* Recompute view_proj from the camera's current parameters. */
void camera_update(struct camera *cam);

#endif /* CAMERA_H */
