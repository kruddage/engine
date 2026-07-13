/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * mesh_script — the mesh scripting path end to end: boot the real s7 image
 * (which loads the embedded mesh_script.scm), call mesh_script_generate()
 * against a hand-authored (mesh ...) source and the built-in grid script, and
 * check the returned mesh_blob satisfies the same geometric invariants
 * primitive_test.c checks for the four built-in primitives.
 */
#include "mesh_script.h"
#include "builtin_mesh_scripts.h"

#include "script.h"
#include "log.h"
#include "memory.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define EPS 1.0e-5f

static const struct memory_api test_mem_impl = {
	mem_alloc, mem_alloc_zero, mem_free,
	mem_pool_create, mem_pool_alloc, mem_pool_free, mem_pool_destroy,
};
static const struct memory_api *g_mem = &test_mem_impl;

static float vlen(const float *v)
{
	return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

/*
 * A single unit-quad mesh, hand-authored the way an author would write one:
 * exercises the general (mesh NAME (generate () (cons VERTS INDICES))) shape
 * with a literal, loop-free body rather than the built-in's nested loops.
 */
static const char *QUAD_SRC =
	"(mesh quad\n"
	"  (generate ()\n"
	"    (cons (list (list -0.5 0.0 -0.5  0.0 1.0 0.0  0.0 0.0)\n"
	"                (list  0.5 0.0 -0.5  0.0 1.0 0.0  1.0 0.0)\n"
	"                (list  0.5 0.0  0.5  0.0 1.0 0.0  1.0 1.0)\n"
	"                (list -0.5 0.0  0.5  0.0 1.0 0.0  0.0 1.0))\n"
	"          (list 0 1 2  0 2 3))))\n";

static void test_hand_authored_quad(void)
{
	struct mesh_blob         *b;
	const struct mesh_vertex *v;
	const uint16_t            *idx;
	uint32_t                   size = 0;
	uint32_t                   i;

	b = mesh_script_generate(QUAD_SRC, NULL, 0, g_mem, &size);
	assert(b != NULL);
	assert(size == mesh_blob_size(4, 6));
	assert(b->magic        == MESH_BLOB_MAGIC);
	assert(b->vertex_count == 4);
	assert(b->index_count  == 6);
	assert(b->index_format == 0);

	v   = mesh_blob_vertices(b);
	idx = mesh_blob_indices(b);
	for (i = 0; i < 6; i++)
		assert(idx[i] < 4);
	for (i = 0; i < 4; i++) {
		assert(v[i].position[0] >= -0.5f - EPS && v[i].position[0] <= 0.5f + EPS);
		assert(v[i].position[2] >= -0.5f - EPS && v[i].position[2] <= 0.5f + EPS);
		assert(fabsf(vlen(v[i].normal) - 1.0f) <= EPS);
	}
	/* The winding an author literally wrote: (0 1 2)(0 2 3). */
	assert(idx[0] == 0 && idx[1] == 1 && idx[2] == 2);
	assert(idx[3] == 0 && idx[4] == 2 && idx[5] == 3);

	g_mem->free(b);
}

/*
 * The built-in grid script — 4x4 subdivisions of a unit plane, the shape a
 * fixed C primitive can't parameterize the way a loop-driven script can.
 */
static void test_builtin_grid(void)
{
	struct mesh_blob         *b;
	const struct mesh_vertex *v;
	const uint16_t            *idx;
	uint32_t                   size = 0;
	uint32_t                   i;

	b = mesh_script_generate(GRID_MESH_SCRIPT_SRC, NULL, 0, g_mem, &size);
	assert(b != NULL);
	assert(size == mesh_blob_size(25, 96));
	assert(b->vertex_count == 25);
	assert(b->index_count  == 96);

	v   = mesh_blob_vertices(b);
	idx = mesh_blob_indices(b);
	for (i = 0; i < 96; i++)
		assert(idx[i] < 25);
	for (i = 0; i < 25; i++) {
		assert(v[i].position[0] >= -0.5f - EPS && v[i].position[0] <= 0.5f + EPS);
		assert(v[i].position[1] >= -0.5f - EPS && v[i].position[1] <= 0.5f + EPS);
		assert(v[i].position[2] >= -0.5f - EPS && v[i].position[2] <= 0.5f + EPS);
		assert(fabsf(v[i].normal[1] - 1.0f) <= EPS);
	}

	g_mem->free(b);
}

/*
 * Every swept or parametric built-in shares one contract: a non-NULL blob of
 * the expected vertex/index counts, an index count that is a whole number of
 * triangles, every index in range, and every normal unit length. gen_checked
 * verifies that and hands the blob back so a per-shape test can add its own
 * bounds checks. The counts are the geometry's fingerprint — they change only
 * when the profile/resolution in builtin_mesh_scripts.h does.
 */
static struct mesh_blob *gen_checked(const char *src, uint32_t exp_v,
				     uint32_t exp_i)
{
	struct mesh_blob         *b;
	const struct mesh_vertex *v;
	const uint16_t           *idx;
	uint32_t                   size = 0;
	uint32_t                   i;

	b = mesh_script_generate(src, NULL, 0, g_mem, &size);
	assert(b != NULL);
	assert(size == mesh_blob_size(exp_v, exp_i));
	assert(b->vertex_count == exp_v);
	assert(b->index_count  == exp_i);
	assert(b->index_count % 3 == 0);

	v   = mesh_blob_vertices(b);
	idx = mesh_blob_indices(b);
	for (i = 0; i < exp_i; i++)
		assert(idx[i] < exp_v);
	for (i = 0; i < exp_v; i++)
		assert(fabsf(vlen(v[i].normal) - 1.0f) <= EPS);
	return b;
}

/* Radius of a vertex from the Y axis — the swept prims' natural extent. */
static float radius_xz(const struct mesh_vertex *v)
{
	return sqrtf(v->position[0] * v->position[0]
		     + v->position[2] * v->position[2]);
}

/*
 * cylinder — the six-point lathe: radius 0.5, height 1. Every vertex sits
 * within the 0.5 radius and the ±0.5 height, and the two zero-height crease
 * rings drop out so the index count is exactly the three real segments.
 */
static void test_builtin_cylinder(void)
{
	struct mesh_blob         *b = gen_checked(CYLINDER_MESH_SCRIPT_SRC, 150, 432);
	const struct mesh_vertex *v = mesh_blob_vertices(b);
	uint32_t                   i;

	for (i = 0; i < 150; i++) {
		assert(radius_xz(&v[i]) <= 0.5f + EPS);
		assert(fabsf(v[i].position[1]) <= 0.5f + EPS);
	}
	g_mem->free(b);
}

/* cone — base radius 0.5 tapering to an apex at r=0, height 1. */
static void test_builtin_cone(void)
{
	struct mesh_blob         *b = gen_checked(CONE_MESH_SCRIPT_SRC, 100, 288);
	const struct mesh_vertex *v = mesh_blob_vertices(b);
	uint32_t                   i;

	for (i = 0; i < 100; i++) {
		assert(radius_xz(&v[i]) <= 0.5f + EPS);
		assert(fabsf(v[i].position[1]) <= 0.5f + EPS);
	}
	g_mem->free(b);
}

/*
 * disc — a flat circle of radius 0.5 on the XZ plane: zero height, every vertex
 * within the radius, and every normal pointing straight up.
 */
static void test_builtin_disc(void)
{
	struct mesh_blob         *b = gen_checked(DISC_MESH_SCRIPT_SRC, 50, 144);
	const struct mesh_vertex *v = mesh_blob_vertices(b);
	uint32_t                   i;

	for (i = 0; i < 50; i++) {
		assert(radius_xz(&v[i]) <= 0.5f + EPS);
		assert(fabsf(v[i].position[1]) <= EPS);
		assert(fabsf(v[i].normal[1] - 1.0f) <= EPS);
	}
	g_mem->free(b);
}

/*
 * torus — a tube of minor radius 0.15 orbiting at major radius 0.35, so every
 * vertex lies in the annulus [0.20, 0.50] and within ±0.15 of the XZ plane. No
 * vertex ever reaches the axis (the one prim with no pole).
 */
static void test_builtin_torus(void)
{
	struct mesh_blob         *b = gen_checked(TORUS_MESH_SCRIPT_SRC, 425, 2304);
	const struct mesh_vertex *v = mesh_blob_vertices(b);
	uint32_t                   i;

	for (i = 0; i < 425; i++) {
		float r = radius_xz(&v[i]);

		assert(r >= 0.2f - EPS && r <= 0.5f + EPS);
		assert(fabsf(v[i].position[1]) <= 0.15f + EPS);
	}
	g_mem->free(b);
}

/*
 * capsule — radius 0.5, a unit cylindrical middle capped by two hemispheres, so
 * it spans the full ±1 in height while never exceeding the 0.5 radius. The caps
 * really do reach y = ±1.
 */
static void test_builtin_capsule(void)
{
	struct mesh_blob         *b = gen_checked(CAPSULE_MESH_SCRIPT_SRC, 450, 2448);
	const struct mesh_vertex *v = mesh_blob_vertices(b);
	uint32_t                   i;
	float                      maxy = 0.0f;

	for (i = 0; i < 450; i++) {
		float ay = fabsf(v[i].position[1]);

		assert(radius_xz(&v[i]) <= 0.5f + EPS);
		assert(ay <= 1.0f + EPS);
		if (ay > maxy)
			maxy = ay;
	}
	assert(fabsf(maxy - 1.0f) <= EPS);
	g_mem->free(b);
}

/*
 * superquadric — with the default exponents (e1=e2=1) a superellipsoid IS an
 * ellipsoid, and ours is the unit sphere scaled to radius 0.5: every vertex is
 * distance 0.5 from the origin. The finite-difference normals are unit too
 * (checked in gen_checked), including the pole fallback.
 */
static void test_builtin_superquadric_default_is_ellipsoid(void)
{
	struct mesh_blob         *b = gen_checked(SUPERQUADRIC_MESH_SCRIPT_SRC, 825, 4608);
	const struct mesh_vertex *v = mesh_blob_vertices(b);
	uint32_t                   i;

	for (i = 0; i < 825; i++)
		assert(fabsf(vlen(v[i].position) - 0.5f) <= 1.0e-3f);
	g_mem->free(b);
}

/*
 * The exponents reshape the surface: e1=e2=0.3 pinches the ellipsoid toward a
 * box, whose corners sit farther from the centre than the 0.5-radius sphere —
 * proof the (params ...) override drives mesh-param-surface, the geometry twin
 * of test_box_params_reshape.
 */
static void test_superquadric_params_reshape(void)
{
	const float               boxish[2] = { 0.3f, 0.3f }; /* e1 e2 */
	struct mesh_blob         *b;
	const struct mesh_vertex *v;
	uint32_t                   i;
	float                      maxr = 0.0f;

	b = mesh_script_generate(SUPERQUADRIC_MESH_SCRIPT_SRC,
				 (const uint8_t *)boxish, sizeof(boxish), g_mem, NULL);
	assert(b != NULL);
	assert(b->vertex_count == 825);
	v = mesh_blob_vertices(b);
	for (i = 0; i < 825; i++) {
		float r = vlen(v[i].position);

		if (r > maxr)
			maxr = r;
	}
	assert(maxr > 0.55f); /* corners push past the ellipsoid's 0.5 */
	g_mem->free(b);
}

/*
 * heightfield — a unit XZ patch displaced in Y. Default relief is non-flat but
 * bounded by the amplitude (0.15), and x/z stay within the ±0.5 footprint.
 */
static void test_builtin_heightfield(void)
{
	struct mesh_blob         *b = gen_checked(HEIGHTFIELD_MESH_SCRIPT_SRC, 625, 3456);
	const struct mesh_vertex *v = mesh_blob_vertices(b);
	uint32_t                   i;
	float                      maxy = 0.0f;

	for (i = 0; i < 625; i++) {
		float ay = fabsf(v[i].position[1]);

		assert(v[i].position[0] >= -0.5f - EPS && v[i].position[0] <= 0.5f + EPS);
		assert(v[i].position[2] >= -0.5f - EPS && v[i].position[2] <= 0.5f + EPS);
		if (ay > maxy)
			maxy = ay;
	}
	assert(maxy > 0.01f);        /* the default terrain has relief   */
	assert(maxy <= 0.15f + EPS); /* bounded by the amp-0.15 envelope */
	g_mem->free(b);
}

/*
 * Amplitude 0 collapses the heightfield to a perfectly flat plane — the param
 * genuinely scales the displacement, not just decorates the asset.
 */
static void test_heightfield_amp_flattens(void)
{
	const float               flat[1] = { 0.0f }; /* amp = 0 */
	struct mesh_blob         *b;
	const struct mesh_vertex *v;
	uint32_t                   i;

	b = mesh_script_generate(HEIGHTFIELD_MESH_SCRIPT_SRC,
				 (const uint8_t *)flat, sizeof(flat), g_mem, NULL);
	assert(b != NULL);
	v = mesh_blob_vertices(b);
	for (i = 0; i < 625; i++)
		assert(fabsf(v[i].position[1]) <= EPS);
	g_mem->free(b);
}

/*
 * Calling generate a second time for the exact same source must produce an
 * equivalent mesh (the *mesh-scripts* cache in mesh_script.scm returns the
 * same result object rather than faulting or drifting on re-evaluation).
 */
static void test_repeat_call_is_stable(void)
{
	struct mesh_blob *a, *b;

	a = mesh_script_generate(QUAD_SRC, NULL, 0, g_mem, NULL);
	b = mesh_script_generate(QUAD_SRC, NULL, 0, g_mem, NULL);
	assert(a != NULL && b != NULL);
	assert(a->vertex_count == b->vertex_count);
	assert(a->index_count  == b->index_count);
	g_mem->free(a);
	g_mem->free(b);
}

/*
 * A source that isn't a well-formed (mesh ...) form, or whose generate body
 * faults, must not crash the caller — mesh-script-generate's catch degrades
 * to an empty result, which marshals to "no mesh" (NULL), not a partial blob.
 */
static void test_malformed_source_yields_no_mesh(void)
{
	assert(mesh_script_generate("(not-a-mesh-form)", NULL, 0, g_mem, NULL) == NULL);
	assert(mesh_script_generate("(mesh broken (generate () (car '())))",
				    NULL, 0, g_mem, NULL) == NULL);
	assert(mesh_script_generate(NULL, NULL, 0, g_mem, NULL) == NULL);
}

/*
 * The built-in box declares a (params width height depth) clause. It must report
 * three tight-packed float fields, in order, each defaulting to 1.0 — the same
 * shape script_entity_params reports for a script, so one marshaller and one set
 * of editor widgets serve meshes too.
 */
static void test_box_params_declared(void)
{
	struct shader_param p[8];
	uint32_t            total = 0;
	int                 n;

	n = script_mesh_params(BOX_MESH_SCRIPT_SRC, p, 8, &total);
	assert(n == 3);
	assert(total == 3 * sizeof(float)); /* tight-packed, no std140 padding */
	assert(strcmp(p[0].name, "width")  == 0 && p[0].components == 1 && p[0].offset == 0);
	assert(strcmp(p[1].name, "height") == 0 && p[1].offset == 4);
	assert(strcmp(p[2].name, "depth")  == 0 && p[2].offset == 8);
	assert(p[0].default_count == 1 && fabsf(p[0].edit_default[0] - 1.0f) <= EPS);
}

/*
 * The heart of parameterized meshes: one mesh source, two param overrides, two
 * different geometries. With no override the box is the unit cube (positions in
 * ±0.5); with width=2 its x half-extent doubles to 1.0 while depth=1 leaves z at
 * ±0.5 — proof the generate body reads (param ...) and the host resolves the
 * override into it.
 */
static void test_box_params_reshape(void)
{
	const float               wide[3] = { 2.0f, 1.0f, 1.0f }; /* w h d */
	struct mesh_blob         *base, *box;
	const struct mesh_vertex *v;
	uint32_t                   i;
	float                      maxx;

	base = mesh_script_generate(BOX_MESH_SCRIPT_SRC, NULL, 0, g_mem, NULL);
	assert(base != NULL);
	assert(base->vertex_count == 24 && base->index_count == 36);
	v = mesh_blob_vertices(base);
	for (i = 0; i < 24; i++)
		assert(v[i].position[0] >= -0.5f - EPS && v[i].position[0] <= 0.5f + EPS);
	g_mem->free(base);

	box = mesh_script_generate(BOX_MESH_SCRIPT_SRC, (const uint8_t *)wide,
				   sizeof(wide), g_mem, NULL);
	assert(box != NULL);
	assert(box->vertex_count == 24 && box->index_count == 36);
	v = mesh_blob_vertices(box);
	maxx = 0.0f;
	for (i = 0; i < 24; i++) {
		float ax = fabsf(v[i].position[0]);

		if (ax > maxx)
			maxx = ax;
		assert(v[i].position[2] >= -0.5f - EPS && v[i].position[2] <= 0.5f + EPS);
	}
	assert(fabsf(maxx - 1.0f) <= EPS); /* width 2 -> half-extent 1.0 */
	g_mem->free(box);
}

/*
 * A short override (only width) fills the fields it covers and leaves the rest at
 * their declared defaults — the override ⊕ defaults contract, so a partial edit
 * never zeroes the params it didn't touch.
 */
static void test_box_partial_override_keeps_defaults(void)
{
	const float               only_w[1] = { 2.0f };
	struct mesh_blob         *box;
	const struct mesh_vertex *v;
	uint32_t                   i;
	float                      maxx = 0.0f, maxy = 0.0f;

	box = mesh_script_generate(BOX_MESH_SCRIPT_SRC, (const uint8_t *)only_w,
				   sizeof(only_w), g_mem, NULL);
	assert(box != NULL);
	v = mesh_blob_vertices(box);
	for (i = 0; i < 24; i++) {
		float ax = fabsf(v[i].position[0]);
		float ay = fabsf(v[i].position[1]);

		if (ax > maxx)
			maxx = ax;
		if (ay > maxy)
			maxy = ay;
	}
	assert(fabsf(maxx - 1.0f) <= EPS); /* width 2 applied      */
	assert(fabsf(maxy - 0.5f) <= EPS); /* height default 1 kept */
	g_mem->free(box);
}

int main(void)
{
	mem_init();
	log_init();
	script_init(); /* loads the embedded mesh_script.scm image */

	test_hand_authored_quad();
	test_builtin_grid();
	test_builtin_cylinder();
	test_builtin_cone();
	test_builtin_disc();
	test_builtin_torus();
	test_builtin_capsule();
	test_builtin_superquadric_default_is_ellipsoid();
	test_superquadric_params_reshape();
	test_builtin_heightfield();
	test_heightfield_amp_flattens();
	test_repeat_call_is_stable();
	test_malformed_source_yields_no_mesh();
	test_box_params_declared();
	test_box_params_reshape();
	test_box_partial_override_keeps_defaults();

	printf("mesh_script tests passed\n");
	return 0;
}
