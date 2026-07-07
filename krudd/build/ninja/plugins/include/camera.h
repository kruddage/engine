/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CAMERA_H
#define CAMERA_H

#include "math_types.h"

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
