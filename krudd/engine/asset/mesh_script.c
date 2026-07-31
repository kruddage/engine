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
#include <string.h>

#define MESH_SCRIPT_MAX_PARAMS 32

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

/* The default of param component K, honouring an authored (default V ...) first
 * and otherwise the field's edit hint — the same resolution es_param_default
 * makes for entity scripts, so an un-overridden mesh and the editor agree. */
static float ms_param_default(const struct shader_param *p, uint32_t k)
{
	if (k < p->default_count)
		return p->edit_default[k];
	if (strcmp(p->edit, "color") == 0)
		return 1.0f;
	if (strcmp(p->edit, "range") == 0)
		return p->edit_min;
	return 0.0f;
}

/*
 * Resolve mesh SRC's declared params against its override BLOB (blen bytes, or
 * NULL for none) into an ((name . value) ...) alist — a real for a scalar param,
 * a list of reals for a vector. Each field reads from the override where present
 * and long enough, else from its default. This is the override ⊕ defaults the
 * generator sees through (param ...); it mirrors entity_script.c's
 * es_param_values so a mesh and a script resolve their params identically.
 */
static s7_pointer ms_param_values(s7_scheme *sc, const char *src,
				  const uint8_t *blob, uint32_t blen)
{
	struct shader_param p[MESH_SCRIPT_MAX_PARAMS];
	int                 n, i;
	s7_pointer          out = s7_nil(sc);

	if (!src)
		return out;
	n = script_mesh_params(src, p, MESH_SCRIPT_MAX_PARAMS, NULL);
	for (i = n - 1; i >= 0; i--) {
		uint32_t   c = p[i].components > 4 ? 4 : p[i].components;
		float      v[4];
		uint32_t   k;
		s7_pointer val;

		for (k = 0; k < c; k++)
			v[k] = ms_param_default(&p[i], k);
		if (blob && p[i].offset + c * sizeof(float) <= blen)
			memcpy(v, blob + p[i].offset, c * sizeof(float));
		if (c == 1) {
			val = s7_make_real(sc, v[0]);
		} else {
			val = s7_nil(sc);
			for (k = c; k > 0; k--)
				val = s7_cons(sc, s7_make_real(sc, v[k - 1]),
					      val);
		}
		out = s7_cons(sc,
			      s7_cons(sc, s7_make_symbol(sc, p[i].name), val),
			      out);
	}
	return out;
}

struct mesh_blob *mesh_script_generate(const char *src,
				       const uint8_t *params, uint32_t plen,
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

	res = s7_call(sc, fn, s7_list(sc, 2, s7_make_string(sc, src),
				      ms_param_values(sc, src, params, plen)));
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
