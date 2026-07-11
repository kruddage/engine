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

	b = mesh_script_generate(QUAD_SRC, g_mem, &size);
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

	b = mesh_script_generate(GRID_MESH_SCRIPT_SRC, g_mem, &size);
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
 * Calling generate a second time for the exact same source must produce an
 * equivalent mesh (the *mesh-scripts* cache in mesh_script.scm returns the
 * same result object rather than faulting or drifting on re-evaluation).
 */
static void test_repeat_call_is_stable(void)
{
	struct mesh_blob *a, *b;

	a = mesh_script_generate(QUAD_SRC, g_mem, NULL);
	b = mesh_script_generate(QUAD_SRC, g_mem, NULL);
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
	assert(mesh_script_generate("(not-a-mesh-form)", g_mem, NULL) == NULL);
	assert(mesh_script_generate("(mesh broken (generate () (car '())))",
				    g_mem, NULL) == NULL);
	assert(mesh_script_generate(NULL, g_mem, NULL) == NULL);
}

int main(void)
{
	mem_init();
	log_init();
	script_init(); /* loads the embedded mesh_script.scm image */

	test_hand_authored_quad();
	test_builtin_grid();
	test_repeat_call_is_stable();
	test_malformed_source_yields_no_mesh();

	printf("mesh_script tests passed\n");
	return 0;
}
