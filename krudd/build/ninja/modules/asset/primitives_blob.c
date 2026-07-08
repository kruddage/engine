/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * primitive_generate — pack the Scheme-generated geometry into a mesh_blob.
 *
 * The geometry itself lives in krudd/build/modules/primitives.scm and reaches C
 * through the generated primitives_gen.h ABI: primitive_vertices() and
 * primitive_indices() marshal one primitive's vertex and index arrays out of
 * the s7 image. This driver keeps primitives.c's original signature and does
 * the part that has no business in Scheme — allocate the single opaque byte
 * buffer the asset catalog hands out and lay the header + vertices + indices
 * into it, using the mesh.h helpers.
 */
#include "primitives.h"
#include "primitives_gen.h"
#include "mesh.h"

#include <string.h>

/*
 * The marshaled prim_vertex is the same 32-byte "position, normal, uv0" record
 * as mesh_vertex, so a filled prim_vertex array copies straight into the blob.
 */
_Static_assert(sizeof(struct prim_vertex) == sizeof(struct mesh_vertex),
	       "prim_vertex must match mesh_vertex layout");

void *primitive_generate(enum primitive_kind kind,
			 const struct memory_api *mem, uint32_t *out_size)
{
	struct prim_vertex *verts;
	uint16_t           *indices;
	struct mesh_blob   *b;
	uint32_t            size;
	int32_t             vcount;
	int32_t             icount;

	if (!mem)
		return NULL;

	/* Scratch sized to the largest primitive (the sphere); freed below. */
	verts   = mem->alloc((size_t)PRIM_VERTS_MAX * sizeof(*verts));
	indices = mem->alloc((size_t)PRIM_INDICES_MAX * sizeof(*indices));
	if (!verts || !indices) {
		mem->free(verts);
		mem->free(indices);
		return NULL;
	}

	vcount = primitive_vertices((int32_t)kind, verts, PRIM_VERTS_MAX);
	icount = primitive_indices((int32_t)kind, indices, PRIM_INDICES_MAX);
	if (vcount <= 0) { /* unknown kind (or image failure) yields no vertices */
		mem->free(verts);
		mem->free(indices);
		return NULL;
	}

	size = mesh_blob_size((uint32_t)vcount, (uint32_t)icount);
	b = mem->alloc(size);
	if (!b) {
		mem->free(verts);
		mem->free(indices);
		return NULL;
	}
	b->magic        = MESH_BLOB_MAGIC;
	b->vertex_count = (uint32_t)vcount;
	b->index_count  = (uint32_t)icount;
	b->index_format = 0; /* uint16 */

	memcpy((void *)(b + 1), verts, (size_t)vcount * sizeof(struct mesh_vertex));
	memcpy((uint16_t *)((struct mesh_vertex *)(b + 1) + vcount), indices,
	       (size_t)icount * sizeof(uint16_t));

	mem->free(verts);
	mem->free(indices);

	if (out_size)
		*out_size = size;
	return b;
}
