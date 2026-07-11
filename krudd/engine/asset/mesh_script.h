/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MESH_SCRIPT_H
#define MESH_SCRIPT_H

#include "mesh.h"
#include "memory_api.h"

/*
 * mesh_script — the bridge between an ASSET_TYPE_MESH_SCRIPT asset's Scheme
 * source and a real mesh_blob, the same shape primitive_generate() (see
 * primitives.h) hands back for the four built-in primitives.
 *
 * Evaluate SRC — a (mesh NAME (generate () ...)) form, see
 * core/mesh_script.scm — against the shared s7 image and marshal its result
 * into a heap-allocated mesh_blob. The caller owns the returned buffer and
 * frees it with mem->free; *out_size receives the total byte count. Returns
 * NULL when the interpreter is unavailable, SRC fails to parse/eval, or its
 * generator yields no geometry — never a partially filled blob.
 */
struct mesh_blob *mesh_script_generate(const char *src,
				       const struct memory_api *mem,
				       uint32_t *out_size);

#endif /* MESH_SCRIPT_H */
