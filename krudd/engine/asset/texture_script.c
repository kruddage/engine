/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * texture_script — host side of the texture scripting layer.
 *
 * Calls the shared s7 image's (texture-script-generate src params w h) — see
 * core/texture_script.scm — and copies its byte-vector result (width*height
 * RGBA8 texels) into a texture_blob. Mirrors mesh_script.c's host/image seam:
 * the pixel logic lives in Scheme, this file only resolves the param override
 * and marshals the flat buffer into the wire format.
 */
#include "texture_script.h"

#include "script.h"

#include "s7.h"

#include <stddef.h>
#include <string.h>

#define TEXTURE_SCRIPT_MAX_PARAMS 32

/* The default of param component K, honouring an authored (default V ...) first
 * and otherwise the field's edit hint — the same resolution ms_param_default
 * makes for meshes, so an un-overridden texture and the editor agree. */
static float ts_param_default(const struct shader_param *p, uint32_t k)
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
 * Resolve texture SRC's declared params against its override BLOB (blen bytes,
 * or NULL for none) into an ((name . value) ...) alist — a real for a scalar
 * param, a list of reals for a vector. Each field reads from the override where
 * present and long enough, else from its default. This is the override ⊕
 * defaults the shade clause sees through (param ...); it mirrors
 * mesh_script.c's ms_param_values so a texture and a mesh resolve params
 * identically.
 */
static s7_pointer ts_param_values(s7_scheme *sc, const char *src,
				  const uint8_t *blob, uint32_t blen)
{
	struct shader_param p[TEXTURE_SCRIPT_MAX_PARAMS];
	int                 n, i;
	s7_pointer          out = s7_nil(sc);

	if (!src)
		return out;
	n = script_texture_params(src, p, TEXTURE_SCRIPT_MAX_PARAMS, NULL);
	for (i = n - 1; i >= 0; i--) {
		uint32_t   c = p[i].components > 4 ? 4 : p[i].components;
		float      v[4];
		uint32_t   k;
		s7_pointer val;

		for (k = 0; k < c; k++)
			v[k] = ts_param_default(&p[i], k);
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

struct texture_blob *texture_script_generate(const char *src,
					     const uint8_t *params,
					     uint32_t plen,
					     uint32_t width, uint32_t height,
					     const struct memory_api *mem,
					     uint32_t *out_size)
{
	s7_scheme           *sc;
	s7_pointer            fn, res;
	uint32_t              pixels_size, total;
	struct texture_blob  *b;

	if (!src || !mem || width == 0 || height == 0)
		return NULL;
	if (width > TEXTURE_SCRIPT_MAX_DIM)
		width = TEXTURE_SCRIPT_MAX_DIM;
	if (height > TEXTURE_SCRIPT_MAX_DIM)
		height = TEXTURE_SCRIPT_MAX_DIM;

	sc = script_s7();
	if (!sc)
		return NULL;
	fn = s7_name_to_value(sc, "texture-script-generate");
	if (!s7_is_procedure(fn))
		return NULL;

	res = s7_call(sc, fn,
		      s7_list(sc, 4, s7_make_string(sc, src),
			      ts_param_values(sc, src, params, plen),
			      s7_make_integer(sc, width),
			      s7_make_integer(sc, height)));
	if (!s7_is_byte_vector(res))
		return NULL;

	pixels_size = texture_blob_pixels_size(width, height);
	if ((uint32_t)s7_vector_length(res) != pixels_size)
		return NULL;

	total = texture_blob_size(width, height);
	b = mem->alloc(total);
	if (!b)
		return NULL;
	b->magic     = TEXTURE_BLOB_MAGIC;
	b->width     = width;
	b->height    = height;
	b->format    = TEXTURE_FORMAT_RGBA8;
	b->mip_count = 1; /* base level only; the GPU expands the chain */

	memcpy((uint8_t *)(b + 1), s7_byte_vector_elements(res), pixels_size);

	if (out_size)
		*out_size = total;
	return b;
}
