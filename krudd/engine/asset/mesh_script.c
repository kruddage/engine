/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * mesh_script — host side of the mesh scripting layer.
 *
 * Calls the shared s7 image's (mesh-script-generate src) — see
 * core/mesh_script.scm — and walks its (VERTS . INDICES) result into a
 * mesh_blob: VERTS a list of 8-element (px py pz nx ny nz u v) lists,
 * INDICES a flat list of vertex-index integers. Mirrors entity_script.c's
 * host/image seam — the geometry logic lives in Scheme, this file only
 * marshals the result into the wire format primitives.c also produces.
 */
#include "mesh_script.h"

#include "script.h"

#include "s7.h"

#include <stddef.h>

/* The K-th real of an 8-element vertex list (px py pz nx ny nz u v), or 0.0
 * for a short or non-numeric entry — a malformed vertex degrades to the
 * origin rather than faulting the caller. */
static float vertex_field(s7_scheme *sc, s7_pointer v, s7_int k)
{
	s7_pointer f = s7_list_ref(sc, v, k);

	return s7_is_number(f) ? (float)s7_number_to_real(sc, f) : 0.0f;
}

static void fill_vertex(s7_scheme *sc, struct mesh_vertex *out, s7_pointer v)
{
	out->position[0] = vertex_field(sc, v, 0);
	out->position[1] = vertex_field(sc, v, 1);
	out->position[2] = vertex_field(sc, v, 2);
	out->normal[0]   = vertex_field(sc, v, 3);
	out->normal[1]   = vertex_field(sc, v, 4);
	out->normal[2]   = vertex_field(sc, v, 5);
	out->uv0[0]       = vertex_field(sc, v, 6);
	out->uv0[1]       = vertex_field(sc, v, 7);
}

struct mesh_blob *mesh_script_generate(const char *src,
				       const struct memory_api *mem,
				       uint32_t *out_size)
{
	s7_scheme          *sc;
	s7_pointer           fn, res, verts, indices, rest;
	uint32_t             vcount, icount, i;
	struct mesh_blob    *b;
	struct mesh_vertex  *v;
	uint16_t            *idx;

	if (!src || !mem)
		return NULL;
	sc = script_s7();
	if (!sc)
		return NULL;
	fn = s7_name_to_value(sc, "mesh-script-generate");
	if (!s7_is_procedure(fn))
		return NULL;

	res = s7_call(sc, fn, s7_list(sc, 1, s7_make_string(sc, src)));
	if (!s7_is_pair(res))
		return NULL;
	verts   = s7_car(res);
	indices = s7_cdr(res);
	vcount  = (uint32_t)s7_list_length(sc, verts);
	icount  = (uint32_t)s7_list_length(sc, indices);
	if (vcount == 0 || icount == 0)
		return NULL;

	b = mem->alloc(mesh_blob_size(vcount, icount));
	if (!b)
		return NULL;
	b->magic        = MESH_BLOB_MAGIC;
	b->vertex_count = vcount;
	b->index_count  = icount;
	b->index_format = 0; /* uint16 */

	v = (struct mesh_vertex *)(b + 1);
	for (rest = verts, i = 0; s7_is_pair(rest); rest = s7_cdr(rest), i++)
		fill_vertex(sc, &v[i], s7_car(rest));

	idx = (uint16_t *)(v + vcount);
	for (rest = indices, i = 0; s7_is_pair(rest); rest = s7_cdr(rest), i++) {
		s7_pointer n = s7_car(rest);

		idx[i] = s7_is_integer(n) ? (uint16_t)s7_integer(n) : 0;
	}

	if (out_size)
		*out_size = mesh_blob_size(vcount, icount);
	return b;
}
