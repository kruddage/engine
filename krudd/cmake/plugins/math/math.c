/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "math_types.h"
#include "world.h"

#include <math.h>
#include <string.h>

static float vec3_dot(const float a[3], const float b[3])
{
	return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}

static void vec3_cross(float out[3], const float a[3], const float b[3])
{
	out[0] = a[1]*b[2] - a[2]*b[1];
	out[1] = a[2]*b[0] - a[0]*b[2];
	out[2] = a[0]*b[1] - a[1]*b[0];
}

static void vec3_normalize(float v[3])
{
	float len = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);

	v[0] /= len;
	v[1] /= len;
	v[2] /= len;
}

void mat4_identity(struct mat4 *out)
{
	memset(out->m, 0, sizeof(out->m));
	out->m[0]  = 1.0f;
	out->m[5]  = 1.0f;
	out->m[10] = 1.0f;
	out->m[15] = 1.0f;
}

void mat4_mul(struct mat4 *out, const struct mat4 *a, const struct mat4 *b)
{
	struct mat4 tmp;
	int         col, row, k;

	for (col = 0; col < 4; col++) {
		for (row = 0; row < 4; row++) {
			float sum = 0.0f;

			for (k = 0; k < 4; k++)
				sum += a->m[k*4+row] * b->m[col*4+k];
			tmp.m[col*4+row] = sum;
		}
	}
	*out = tmp;
}

/*
 * Standard GL perspective: right-handed, depth in NDC [-1, 1].
 * Passing this directly to glUniformMatrix4fv with transpose=GL_FALSE
 * produces the same result as glm::perspective.
 */
void mat4_perspective(struct mat4 *out, float fov_y, float aspect,
		      float near, float far)
{
	float f         = 1.0f / tanf(fov_y * 0.5f);
	float inv_range = 1.0f / (near - far);

	memset(out->m, 0, sizeof(out->m));
	out->m[0]  = f / aspect;
	out->m[5]  = f;
	out->m[10] = (far + near) * inv_range;
	out->m[11] = -1.0f;
	out->m[14] = 2.0f * far * near * inv_range;
}

void mat4_look_at(struct mat4 *out, const float eye[3],
		  const float center[3], const float up[3])
{
	float f[3], s[3], u[3];

	f[0] = center[0] - eye[0];
	f[1] = center[1] - eye[1];
	f[2] = center[2] - eye[2];
	vec3_normalize(f);

	vec3_cross(s, f, up);
	vec3_normalize(s);

	vec3_cross(u, s, f);

	memset(out->m, 0, sizeof(out->m));
	/* column 0: right */
	out->m[0] = s[0];
	out->m[1] = u[0];
	out->m[2] = -f[0];
	/* column 1: corrected up */
	out->m[4] = s[1];
	out->m[5] = u[1];
	out->m[6] = -f[1];
	/* column 2: -forward (GL camera looks along -Z) */
	out->m[8]  = s[2];
	out->m[9]  = u[2];
	out->m[10] = -f[2];
	/* column 3: eye-space translation */
	out->m[12] = -vec3_dot(s, eye);
	out->m[13] = -vec3_dot(u, eye);
	out->m[14] =  vec3_dot(f, eye);
	out->m[15] = 1.0f;
}

void mat4_from_transform(struct mat4 *out, const struct transform *t)
{
	float qx = t->rotation[0];
	float qy = t->rotation[1];
	float qz = t->rotation[2];
	float qw = t->rotation[3];
	float sx = t->scale[0];
	float sy = t->scale[1];
	float sz = t->scale[2];
	float r00, r01, r02, r10, r11, r12, r20, r21, r22;

	r00 = 1.0f - 2.0f*(qy*qy + qz*qz);
	r01 = 2.0f*(qx*qy - qw*qz);
	r02 = 2.0f*(qx*qz + qw*qy);
	r10 = 2.0f*(qx*qy + qw*qz);
	r11 = 1.0f - 2.0f*(qx*qx + qz*qz);
	r12 = 2.0f*(qy*qz - qw*qx);
	r20 = 2.0f*(qx*qz - qw*qy);
	r21 = 2.0f*(qy*qz + qw*qx);
	r22 = 1.0f - 2.0f*(qx*qx + qy*qy);

	memset(out->m, 0, sizeof(out->m));
	/* column 0: world X axis (right), scaled */
	out->m[0] = r00 * sx;
	out->m[1] = r10 * sx;
	out->m[2] = r20 * sx;
	/* column 1: world Y axis (up), scaled */
	out->m[4] = r01 * sy;
	out->m[5] = r11 * sy;
	out->m[6] = r21 * sy;
	/* column 2: world Z axis (forward), scaled */
	out->m[8]  = r02 * sz;
	out->m[9]  = r12 * sz;
	out->m[10] = r22 * sz;
	/* column 3: translation */
	out->m[12] = t->position[0];
	out->m[13] = t->position[1];
	out->m[14] = t->position[2];
	out->m[15] = 1.0f;
}
