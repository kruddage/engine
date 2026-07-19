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

void mat4_clip_z01(struct mat4 *m)
{
	int c;

	/*
	 * Left-multiply by the matrix that maps (x, y, z, w) -> (x, y,
	 * 0.5z + 0.5w, w): the new z row is 0.5 * z row + 0.5 * w row, every
	 * other row unchanged. Column-major, so row r of column c is m[c*4 + r];
	 * row 2 is z, row 3 is w.
	 */
	for (c = 0; c < 4; c++)
		m->m[c*4 + 2] = 0.5f * m->m[c*4 + 2] + 0.5f * m->m[c*4 + 3];
}

/*
 * mat4_perspective is generated from krudd/engine/math/math.scm by krudd's
 * monolang emitter (into ${generated}/math_gen.c) — it is intentionally not
 * hand-written here.
 */

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

void mat4_transform_point(float out[3], const struct mat4 *mm, const float p[3])
{
	const float *m = mm->m;
	float x = m[0]*p[0] + m[4]*p[1] + m[8]*p[2]  + m[12];
	float y = m[1]*p[0] + m[5]*p[1] + m[9]*p[2]  + m[13];
	float z = m[2]*p[0] + m[6]*p[1] + m[10]*p[2] + m[14];

	out[0] = x;
	out[1] = y;
	out[2] = z;
}

/*
 * Cofactor-expansion inverse (the Mesa gluInvertMatrix routine).  The flat
 * indices below match our column-major layout, so the result is the inverse
 * stored in the same convention.
 */
int mat4_inverse(struct mat4 *out, const struct mat4 *in)
{
	const float *m = in->m;
	float        inv[16];
	float        det;
	int          i;

	inv[0] =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15]
		+ m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
	inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15]
		- m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
	inv[8] =  m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15]
		+ m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
	inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14]
		- m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
	inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15]
		- m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
	inv[5] =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15]
		+ m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
	inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15]
		- m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
	inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14]
		+ m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
	inv[2] =  m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15]
		+ m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
	inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15]
		- m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
	inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15]
		+ m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
	inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14]
		- m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
	inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11]
		- m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
	inv[7] =  m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11]
		+ m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
	inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11]
		- m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
	inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10]
		+ m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];

	det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
	if (det > -1e-12f && det < 1e-12f)
		return -1;
	det = 1.0f / det;
	for (i = 0; i < 16; i++)
		out->m[i] = inv[i] * det;
	return 0;
}

/*
 * Unproject a clip-space point (ndc_x, ndc_y, ndc_z, w=1) through the already
 * inverted view*projection, dividing out w.  Fails if the point maps onto the
 * w=0 plane.
 */
static int mat4_unproject(const struct mat4 *inv, float ndc_x, float ndc_y,
			  float ndc_z, float out[3])
{
	const float *m = inv->m;
	float x = m[0]*ndc_x + m[4]*ndc_y + m[8]*ndc_z  + m[12];
	float y = m[1]*ndc_x + m[5]*ndc_y + m[9]*ndc_z  + m[13];
	float z = m[2]*ndc_x + m[6]*ndc_y + m[10]*ndc_z + m[14];
	float w = m[3]*ndc_x + m[7]*ndc_y + m[11]*ndc_z + m[15];

	if (w > -1e-6f && w < 1e-6f)
		return -1;
	w = 1.0f / w;
	out[0] = x * w;
	out[1] = y * w;
	out[2] = z * w;
	return 0;
}

int ray_from_screen(const struct mat4 *view_proj, float sx, float sy,
		    float width, float height, float origin[3], float dir[3])
{
	struct mat4 inv;
	float       ndc_x, ndc_y;
	float       far_pt[3];

	if (width <= 0.0f || height <= 0.0f)
		return -1;
	if (mat4_inverse(&inv, view_proj) != 0)
		return -1;

	/* Pixel -> GL NDC.  Y flips: pixel y grows downward, NDC y upward. */
	ndc_x = sx / width * 2.0f - 1.0f;
	ndc_y = 1.0f - sy / height * 2.0f;

	/* Ray from the near plane (z = -1) toward the far plane (z = +1). */
	if (mat4_unproject(&inv, ndc_x, ndc_y, -1.0f, origin) != 0)
		return -1;
	if (mat4_unproject(&inv, ndc_x, ndc_y, 1.0f, far_pt) != 0)
		return -1;

	dir[0] = far_pt[0] - origin[0];
	dir[1] = far_pt[1] - origin[1];
	dir[2] = far_pt[2] - origin[2];
	vec3_normalize(dir);
	return 0;
}

int ray_tri_intersect(const float origin[3], const float dir[3],
		      const float v0[3], const float v1[3], const float v2[3],
		      float *t_out)
{
	float e1[3], e2[3], pvec[3], qvec[3], tvec[3];
	float det, inv_det, u, v, t;

	e1[0] = v1[0] - v0[0]; e1[1] = v1[1] - v0[1]; e1[2] = v1[2] - v0[2];
	e2[0] = v2[0] - v0[0]; e2[1] = v2[1] - v0[1]; e2[2] = v2[2] - v0[2];

	vec3_cross(pvec, dir, e2);
	det = vec3_dot(e1, pvec);
	if (det > -1e-8f && det < 1e-8f)
		return 0; /* ray parallel to the triangle plane */
	inv_det = 1.0f / det;

	tvec[0] = origin[0] - v0[0];
	tvec[1] = origin[1] - v0[1];
	tvec[2] = origin[2] - v0[2];
	u = vec3_dot(tvec, pvec) * inv_det;
	if (u < 0.0f || u > 1.0f)
		return 0;

	vec3_cross(qvec, tvec, e1);
	v = vec3_dot(dir, qvec) * inv_det;
	if (v < 0.0f || u + v > 1.0f)
		return 0;

	t = vec3_dot(e2, qvec) * inv_det;
	if (t <= 0.0f)
		return 0; /* triangle behind the ray origin */
	*t_out = t;
	return 1;
}
