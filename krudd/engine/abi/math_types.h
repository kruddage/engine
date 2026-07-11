/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MATH_TYPES_H
#define MATH_TYPES_H

/*
 * Shared spatial math primitives used across renderer-adjacent plugins.
 *
 * mat4 is column-major: element at (row, col) lives at m[col * 4 + row].
 * This matches OpenGL's memory convention and may be passed directly to
 * glUniformMatrix4fv with transpose=GL_FALSE.
 *
 * vec3_t and quat_t follow the layout specified in #161.  mat4 is added
 * here as part of #171 so all spatial types share one header.  When #161
 * lands it should extend this file rather than creating a second location.
 */

/* Three-component float vector. */
typedef struct { float x, y, z; }       vec3_t;

/* Unit quaternion, xyzw component order. */
typedef struct { float x, y, z, w; }    quat_t;

/*
 * 4x4 float matrix, column-major, GL-compatible.
 * Element at row r, column c is m[c * 4 + r].
 */
struct mat4 {
	float m[16];
};

struct transform; /* full definition in world.h */

void mat4_identity(struct mat4 *out);

void mat4_mul(struct mat4 *out, const struct mat4 *a,
	      const struct mat4 *b);

/*
 * Perspective projection (right-handed, z maps to GL NDC [-1, 1]).
 * fov_y is the vertical field of view in radians.
 */
void mat4_perspective(struct mat4 *out, float fov_y, float aspect,
		      float near, float far);

/*
 * View matrix from eye position, look-at target, and world-up hint.
 * Equivalent to glm::lookAt / gluLookAt.
 */
void mat4_look_at(struct mat4 *out, const float eye[3],
		  const float center[3], const float up[3]);

/*
 * Model matrix from a world-space transform (TRS: translate * rotate
 * * scale).  struct transform is defined in world.h.
 */
void mat4_from_transform(struct mat4 *out, const struct transform *t);

#endif /* MATH_TYPES_H */
