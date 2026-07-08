/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Geometric invariants every primitive generator must satisfy, run against two
 * backends: the golden C reference (primitives.c, linked as primitives_ref) and
 * the Scheme port (primitives.scm via primitives_blob.c, linked as
 * primitives_scheme). Both build this same source into their own executable and
 * must pass. The C float / Scheme double paths do not agree bit-for-bit on the
 * sphere and pyramid, so the port is proven correct by these invariants (exact
 * counts and indices, unit bounds, sphere radius, unit normals) rather than by
 * byte-for-byte equality — the equivalent of md_parse's "one spec, two proofs".
 */
#include "primitives.h"
#include "mesh.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define EPS 1.0e-5f

/* A malloc-backed memory_api: the generators only use alloc and free. */
static const struct memory_api test_mem = {
	.alloc = malloc,
	.free  = free,
};

static float vlen(const float *v)
{
	return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

static void check(const char *name, enum primitive_kind kind,
		  uint32_t vcount, uint32_t icount, int is_sphere)
{
	const struct mesh_blob   *b;
	const struct mesh_vertex *v;
	const uint16_t           *idx;
	uint32_t                  size = 0;
	uint32_t                  i;

	b = primitive_generate(kind, &test_mem, &size);
	assert(b != NULL);

	/* Structure: exact, both backends. */
	assert(size == mesh_blob_size(vcount, icount));
	assert(b->magic        == MESH_BLOB_MAGIC);
	assert(b->vertex_count == vcount);
	assert(b->index_count  == icount);
	assert(b->index_format == 0);

	v   = mesh_blob_vertices(b);
	idx = mesh_blob_indices(b);

	/* Every index addresses a real vertex. */
	for (i = 0; i < icount; i++)
		assert(idx[i] < vcount);

	for (i = 0; i < vcount; i++) {
		/* Unit-sized, centred on the origin: bounds -0.5 .. 0.5. */
		assert(v[i].position[0] >= -0.5f - EPS && v[i].position[0] <= 0.5f + EPS);
		assert(v[i].position[1] >= -0.5f - EPS && v[i].position[1] <= 0.5f + EPS);
		assert(v[i].position[2] >= -0.5f - EPS && v[i].position[2] <= 0.5f + EPS);

		/* Normals are unit length. */
		assert(fabsf(vlen(v[i].normal) - 1.0f) <= EPS);

		if (is_sphere) {
			/* Every vertex sits on the radius-0.5 shell, and its
			 * normal points straight out (position * 2). */
			assert(fabsf(vlen(v[i].position) - 0.5f) <= EPS);
			assert(fabsf(v[i].normal[0] - v[i].position[0] * 2.0f) <= EPS);
			assert(fabsf(v[i].normal[1] - v[i].position[1] * 2.0f) <= EPS);
			assert(fabsf(v[i].normal[2] - v[i].position[2] * 2.0f) <= EPS);
		}
	}

	free((void *)b);
	printf("PASS: %s (%u verts / %u indices)\n", name, vcount, icount);
}

int main(void)
{
	check("cube",    PRIMITIVE_CUBE,    24,  36,  0);
	check("sphere",  PRIMITIVE_SPHERE,  561, 2880, 1);
	check("plane",   PRIMITIVE_PLANE,   4,   6,   0);
	check("pyramid", PRIMITIVE_PYRAMID, 16,  18,  0);

	/* An out-of-range kind yields no mesh. */
	assert(primitive_generate((enum primitive_kind)99, &test_mem, NULL) == NULL);

	printf("primitive tests passed\n");
	return 0;
}
