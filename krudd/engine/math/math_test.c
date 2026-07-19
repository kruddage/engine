/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "camera.h"
#include "world.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define PI 3.14159265358979323846f

static int feq(float a, float b)
{
	float d = a - b;

	if (d < 0.0f)
		d = -d;
	return d < 1e-4f;
}

/* out = m * v, column-major (row r of column c is m[c*4 + r]). */
static void mul4(float out[4], const struct mat4 *m, const float v[4])
{
	int r, c;

	for (r = 0; r < 4; r++) {
		float s = 0.0f;

		for (c = 0; c < 4; c++)
			s += m->m[c*4 + r] * v[c];
		out[r] = s;
	}
}

static void test_identity(void)
{
	struct mat4 m;
	int         i;

	mat4_identity(&m);
	for (i = 0; i < 16; i++) {
		int diag = (i == 0 || i == 5 || i == 10 || i == 15);

		assert(feq(m.m[i], diag ? 1.0f : 0.0f));
	}
}

static void test_mul_identity(void)
{
	struct mat4 a, b, r;
	int         i;

	mat4_identity(&a);
	mat4_identity(&b);
	mat4_mul(&r, &a, &b);
	for (i = 0; i < 16; i++) {
		int diag = (i == 0 || i == 5 || i == 10 || i == 15);

		assert(feq(r.m[i], diag ? 1.0f : 0.0f));
	}
}

/* T(1,0,0) * T(0,2,0) should give translation (1,2,0). */
static void test_mul_translate(void)
{
	struct mat4 t1, t2, r;

	mat4_identity(&t1);
	t1.m[12] = 1.0f;
	mat4_identity(&t2);
	t2.m[13] = 2.0f;

	mat4_mul(&r, &t1, &t2);
	assert(feq(r.m[12], 1.0f));
	assert(feq(r.m[13], 2.0f));
	assert(feq(r.m[14], 0.0f));
	assert(feq(r.m[15], 1.0f));
}

static void test_perspective(void)
{
	struct mat4 p;
	float       near = 0.1f, far = 100.0f;

	/* fov_y = pi/2, aspect = 1 => f = 1/tan(pi/4) = 1 */
	mat4_perspective(&p, PI * 0.5f, 1.0f, near, far);

	assert(feq(p.m[0],  1.0f));
	assert(feq(p.m[5],  1.0f));
	assert(feq(p.m[10], (far + near) / (near - far)));
	assert(feq(p.m[11], -1.0f));
	assert(feq(p.m[14], 2.0f * far * near / (near - far)));

	/* row 3 of cols 0–2 must be zero */
	assert(feq(p.m[3],  0.0f));
	assert(feq(p.m[7],  0.0f));
	assert(feq(p.m[15], 0.0f));
}

/*
 * Camera looking along -Z from z=5: right=(1,0,0), up=(0,1,0),
 * -fwd=(0,0,1), translation z=-5.
 */
static void test_look_at(void)
{
	struct mat4 v;
	float eye[3]    = { 0.0f, 0.0f, 5.0f };
	float center[3] = { 0.0f, 0.0f, 0.0f };
	float up[3]     = { 0.0f, 1.0f, 0.0f };

	mat4_look_at(&v, eye, center, up);

	assert(feq(v.m[0],  1.0f));   /* right.x  */
	assert(feq(v.m[5],  1.0f));   /* up.y     */
	assert(feq(v.m[10], 1.0f));   /* -fwd.z   */
	assert(feq(v.m[12], 0.0f));
	assert(feq(v.m[13], 0.0f));
	assert(feq(v.m[14], -5.0f));  /* dot(fwd, eye) = -5 */
	assert(feq(v.m[15], 1.0f));
}

/* Identity rotation + translation (1,2,3) + uniform scale 1. */
static void test_from_transform_identity(void)
{
	struct mat4      m;
	struct transform t;
	int              i;

	t.position[0] = 1.0f;
	t.position[1] = 2.0f;
	t.position[2] = 3.0f;
	t.rotation[0] = 0.0f;
	t.rotation[1] = 0.0f;
	t.rotation[2] = 0.0f;
	t.rotation[3] = 1.0f; /* identity quaternion (0,0,0,1) */
	t.scale[0] = t.scale[1] = t.scale[2] = 1.0f;

	mat4_from_transform(&m, &t);

	assert(feq(m.m[0],  1.0f));
	assert(feq(m.m[5],  1.0f));
	assert(feq(m.m[10], 1.0f));
	assert(feq(m.m[15], 1.0f));
	assert(feq(m.m[12], 1.0f));
	assert(feq(m.m[13], 2.0f));
	assert(feq(m.m[14], 3.0f));
	for (i = 0; i < 12; i++) {
		int diag = (i == 0 || i == 5 || i == 10);

		if (!diag)
			assert(feq(m.m[i], 0.0f));
	}
}

/* 90 degrees CCW around Z: X-axis basis vector maps to +Y. */
static void test_from_transform_rot_z90(void)
{
	const float      h = 0.70710678f; /* sin/cos of 45 degrees */
	struct mat4      m;
	struct transform t;

	t.position[0] = t.position[1] = t.position[2] = 0.0f;
	t.rotation[0] = 0.0f;
	t.rotation[1] = 0.0f;
	t.rotation[2] = h;    /* z */
	t.rotation[3] = h;    /* w */
	t.scale[0] = t.scale[1] = t.scale[2] = 1.0f;

	mat4_from_transform(&m, &t);

	/* column 0 (X basis → world Y): */
	assert(feq(m.m[0],  0.0f));
	assert(feq(m.m[1],  1.0f));
	assert(feq(m.m[2],  0.0f));
	/* column 1 (Y basis → world -X): */
	assert(feq(m.m[4], -1.0f));
	assert(feq(m.m[5],  0.0f));
	/* column 2 (Z basis → world Z, unchanged): */
	assert(feq(m.m[10], 1.0f));
}

/*
 * camera_update: eye=(0,0,5) looking at origin, fov=pi/2, aspect=1.
 * view_proj = proj * view; the view is a pure -5 z-translation so the
 * upper 2x2 of view_proj equals the perspective scale, and w_clip of
 * the last column equals -z_eye = 5.
 */
static void test_camera_view_proj(void)
{
	struct camera cam;
	float         near = 0.1f, far = 100.0f;

	cam.eye[0] = 0.0f; cam.eye[1] = 0.0f; cam.eye[2] = 5.0f;
	cam.target[0] = 0.0f; cam.target[1] = 0.0f; cam.target[2] = 0.0f;
	cam.up[0] = 0.0f; cam.up[1] = 1.0f; cam.up[2] = 0.0f;
	cam.fov_y  = PI * 0.5f;
	cam.aspect = 1.0f;
	cam.near   = near;
	cam.far    = far;

	camera_update(&cam);

	assert(feq(cam.view_proj.m[0],  1.0f));
	assert(feq(cam.view_proj.m[5],  1.0f));
	assert(feq(cam.view_proj.m[10], (far + near) / (near - far)));
	assert(feq(cam.view_proj.m[11], -1.0f));
	/* w-component of the translation column: -z_eye = 5 */
	assert(feq(cam.view_proj.m[15], 5.0f));
}

/* M * inverse(M) == identity for a non-trivial TRS matrix. */
static void test_inverse_roundtrip(void)
{
	const float      h = 0.70710678f; /* 90 deg about Z */
	struct transform t;
	struct mat4      m, inv, prod;
	int              i;

	t.position[0] = 3.0f; t.position[1] = -1.0f; t.position[2] = 2.0f;
	t.rotation[0] = 0.0f; t.rotation[1] = 0.0f;
	t.rotation[2] = h;    t.rotation[3] = h;
	t.scale[0] = 2.0f; t.scale[1] = 0.5f; t.scale[2] = 1.5f;

	mat4_from_transform(&m, &t);
	assert(mat4_inverse(&inv, &m) == 0);
	mat4_mul(&prod, &m, &inv);
	for (i = 0; i < 16; i++) {
		int diag = (i == 0 || i == 5 || i == 10 || i == 15);

		assert(feq(prod.m[i], diag ? 1.0f : 0.0f));
	}
}

/* A singular (all-zero) matrix has no inverse. */
static void test_inverse_singular(void)
{
	struct mat4 zero, inv;

	memset(zero.m, 0, sizeof(zero.m));
	assert(mat4_inverse(&inv, &zero) == -1);
}

static void test_transform_point(void)
{
	struct transform t;
	struct mat4      m;
	float            p[3] = { 1.0f, 0.0f, 0.0f };
	float            out[3];

	/* 90 deg about Z: +X maps to +Y, then translate by (0,0,5). */
	t.position[0] = 0.0f; t.position[1] = 0.0f; t.position[2] = 5.0f;
	t.rotation[0] = 0.0f; t.rotation[1] = 0.0f;
	t.rotation[2] = 0.70710678f; t.rotation[3] = 0.70710678f;
	t.scale[0] = t.scale[1] = t.scale[2] = 1.0f;

	mat4_from_transform(&m, &t);
	mat4_transform_point(out, &m, p);
	assert(feq(out[0], 0.0f));
	assert(feq(out[1], 1.0f));
	assert(feq(out[2], 5.0f));
}

/* Ray straight down -Z through a triangle in the z=0 plane. */
static void test_ray_tri_hit(void)
{
	float v0[3] = { 0.0f, 0.0f, 0.0f };
	float v1[3] = { 1.0f, 0.0f, 0.0f };
	float v2[3] = { 0.0f, 1.0f, 0.0f };
	float o[3]  = { 0.25f, 0.25f, 1.0f };
	float d[3]  = { 0.0f, 0.0f, -1.0f };
	float t     = 0.0f;

	assert(ray_tri_intersect(o, d, v0, v1, v2, &t) == 1);
	assert(feq(t, 1.0f));
}

static void test_ray_tri_miss(void)
{
	float v0[3] = { 0.0f, 0.0f, 0.0f };
	float v1[3] = { 1.0f, 0.0f, 0.0f };
	float v2[3] = { 0.0f, 1.0f, 0.0f };
	float d[3]  = { 0.0f, 0.0f, -1.0f };
	float t     = 0.0f;

	/* Outside the triangle's barycentric span. */
	float outside[3] = { 2.0f, 2.0f, 1.0f };
	assert(ray_tri_intersect(outside, d, v0, v1, v2, &t) == 0);

	/* Over the triangle but aimed away from it. */
	float away_o[3] = { 0.25f, 0.25f, 1.0f };
	float away_d[3] = { 0.0f, 0.0f, 1.0f };
	assert(ray_tri_intersect(away_o, away_d, v0, v1, v2, &t) == 0);
}

/*
 * Screen-center pixel of the eye=(0,0,5), look -Z camera unprojects to a ray
 * aimed along -Z, originating just in front of the eye; a pixel to the right
 * tilts the ray toward +X (screen-right), proving no axis flip.
 */
static void test_ray_from_screen(void)
{
	struct camera cam;
	float         o[3], d[3];

	cam.eye[0] = 0.0f; cam.eye[1] = 0.0f; cam.eye[2] = 5.0f;
	cam.target[0] = 0.0f; cam.target[1] = 0.0f; cam.target[2] = 0.0f;
	cam.up[0] = 0.0f; cam.up[1] = 1.0f; cam.up[2] = 0.0f;
	cam.fov_y  = PI * 0.5f;
	cam.aspect = 1.0f;
	cam.near   = 0.1f;
	cam.far    = 100.0f;
	camera_update(&cam);

	assert(ray_from_screen(&cam.view_proj, 50.0f, 50.0f,
			       100.0f, 100.0f, o, d) == 0);
	assert(feq(d[0], 0.0f));
	assert(feq(d[1], 0.0f));
	assert(feq(d[2], -1.0f));
	assert(feq(o[0], 0.0f));
	assert(feq(o[1], 0.0f));
	assert(o[2] < 5.0f && o[2] > 4.5f); /* near plane, in front of the eye */

	assert(ray_from_screen(&cam.view_proj, 75.0f, 50.0f,
			       100.0f, 100.0f, o, d) == 0);
	assert(d[0] > 0.0f); /* right-of-center pixel leans toward +X */
}

/*
 * mat4_clip_z01 turns a GL-convention projection (NDC z in [-1, 1]) into the
 * [0, 1] convention: the near plane maps to 0 and the far plane to 1, while x,
 * y, and w are left alone.
 */
static void test_clip_z01(void)
{
	const float near = 0.1f, far = 100.0f;
	float pn[4] = { 0.0f, 0.0f, -near, 1.0f };   /* eye-space near-plane point */
	float pf[4] = { 0.0f, 0.0f, -far,  1.0f };   /* eye-space far-plane point  */
	float cn[4], cf[4], zn[4], zf[4];
	struct mat4 p, pz;

	mat4_perspective(&p, PI * 0.5f, 1.0f, near, far);
	pz = p;
	mat4_clip_z01(&pz);

	mul4(cn, &p,  pn);   mul4(cf, &p,  pf);   /* GL convention */
	mul4(zn, &pz, pn);   mul4(zf, &pz, pf);   /* adapted       */

	assert(feq(cn[2] / cn[3], -1.0f));   /* GL: near -> -1 */
	assert(feq(cf[2] / cf[3],  1.0f));   /* GL: far  -> +1 */
	assert(feq(zn[2] / zn[3],  0.0f));   /* adapted: near -> 0 */
	assert(feq(zf[2] / zf[3],  1.0f));   /* adapted: far  -> 1 */

	assert(feq(zn[0], cn[0]));           /* x untouched */
	assert(feq(zn[1], cn[1]));           /* y untouched */
	assert(feq(zn[3], cn[3]));           /* w untouched */
}

int main(void)
{
	test_identity();
	test_mul_identity();
	test_mul_translate();
	test_perspective();
	test_clip_z01();
	test_look_at();
	test_from_transform_identity();
	test_from_transform_rot_z90();
	test_camera_view_proj();
	test_inverse_roundtrip();
	test_inverse_singular();
	test_transform_point();
	test_ray_tri_hit();
	test_ray_tri_miss();
	test_ray_from_screen();

	printf("math tests passed\n");
	return 0;
}
