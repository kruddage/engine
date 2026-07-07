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
 * A UI that knows the drawable pixel size (kruddboard) calls set_viewport each
 * frame; the renderer and every overlay then share one aspect, so a non-1.6
 * canvas no longer skews the projection.
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
};

#endif /* CAMERA_API_H */
