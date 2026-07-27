/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CAMERA_API_H
#define CAMERA_API_H

#include "math_types.h"

/*
 * The "camera" subsystem api — read the live scene camera and keep its
 * projection aspect in sync with the actual viewport.  Published by
 * scene_renderer, which owns the single camera it draws the world with.
 *
 * Editor overlays resolve this to project world-space points to screen space
 * with the same view·projection the renderer uses, so their geometry lands on
 * the rendered meshes.  The transform gizmo (#178) is the first consumer: it
 * reads view_proj to place its handles and eye to size them by distance.
 *
 * A UI that knows the drawable pixel size (the viewport bridge, ui/viewport)
 * calls set_viewport each frame; the renderer and every overlay then share one
 * aspect, so a non-1.6 canvas no longer skews the projection.
 */
struct camera_api {
	/* Copy the current view·projection matrix into *out. */
	void (*get_view_proj)(struct mat4 *out);
	/* Copy the camera eye (world-space position) into out[3]. */
	void (*get_eye)(float out[3]);
	/*
	 * Set the viewport pixel size so the projection aspect matches the live
	 * canvas.  Non-positive dimensions are ignored.
	 */
	void (*set_viewport)(float width, float height);

	/*
	 * Interactive navigation (#697) — orbit / pan / dolly the view from
	 * pointer deltas, for a native editor with no scripted camera to fly it.
	 *
	 * The scene camera normally tracks the scripted "Camera" entity: the
	 * renderer copies that entity's position into the eye every frame. The
	 * FIRST of these calls DETACHES the camera from that script and holds the
	 * user's pose, so a drag is not fought back by the demo's orbit; reset()
	 * reattaches it. A consumer that never navigates never detaches, so the
	 * scripted camera behaves exactly as before.
	 *
	 * All three are no-ops on a NULL/older api, so guard the pointer (as with
	 * set_viewport) before calling.
	 */
	/* Rotate the eye about the fixed target: dyaw about world up, dpitch
	 * about the camera's right axis, both in radians. Pitch is clamped short
	 * of the poles so the view never flips over the top. */
	void (*orbit)(float dyaw, float dpitch);
	/* Slide eye AND target across the view plane, so the framed point tracks
	 * the pointer. dx/dy are viewport fractions (a drag of the full width is
	 * dx = 1); the world distance scales with how far the target sits. */
	void (*pan)(float dx, float dy);
	/* Move the eye along the view direction: amount > 0 dollies toward the
	 * target (zoom in), < 0 away. amount is a fraction of the current
	 * eye→target distance, clamped so the eye never crosses the target. */
	void (*dolly)(float amount);
	/* Reattach to the scripted scene camera, dropping the user's pose. Called
	 * after loading a new scene so its authored camera takes over. */
	void (*reset_view)(void);
};

#endif /* CAMERA_API_H */
