/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PRIMITIVES_H
#define PRIMITIVES_H

#include "mesh.h"
#include "memory_api.h"

#include <stdint.h>

/*
 * Which built-in primitive to generate. The order mirrors the builtin_paths
 * table in asset_plugin.c so a catalog index maps straight to a kind.
 */
enum primitive_kind {
	PRIMITIVE_CUBE = 0,
	PRIMITIVE_SPHERE,
	PRIMITIVE_PLANE,
	PRIMITIVE_PYRAMID,
};

/*
 * Generate a mesh blob (struct mesh_blob header + interleaved mesh_vertex
 * array + uint16 indices) for the given primitive, allocated through mem. The
 * caller owns the buffer and frees it with mem->free; *out_size receives the
 * total byte count. Returns NULL on a bad kind or allocation failure. All
 * primitives are unit-sized, centred on the origin (bounds -0.5 .. 0.5).
 */
void *primitive_generate(enum primitive_kind kind,
			 const struct memory_api *mem, uint32_t *out_size);

#endif /* PRIMITIVES_H */
