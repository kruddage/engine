/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MESH_SCRIPT_H
#define MESH_SCRIPT_H

#include "mesh.h"
#include "memory_api.h"

/*
 * mesh_script — the bridge between an ASSET_TYPE_MESH asset's Scheme source
 * and a real mesh_blob. There is no other kind of mesh asset: every built-in
 * or authored mesh is a (mesh NAME (generate () ...)) form, and this is what
 * turns one into GPU-uploadable geometry.
 *
 * Evaluate SRC — a (mesh NAME [(params ...)] (generate () ...)) form, see
 * core/mesh_script.scm — against the shared s7 image and marshal its result
 * into a heap-allocated mesh_blob. PARAMS (plen bytes, or NULL for none) is the
 * entity's tight-packed override of SRC's declared params; each field is read
 * from the override where present and long enough, else from its declared
 * default, and the resolved values are bound as the generator's (param ...)
 * scope — so the same SRC yields different geometry per override, the way a
 * material's params recolor a shared shader. The caller owns the returned buffer
 * and frees it with mem->free; *out_size receives the total byte count. Returns
 * NULL when the interpreter is unavailable, SRC fails to parse/eval, or its
 * generator yields no geometry — never a partially filled blob.
 */
struct mesh_blob *mesh_script_generate(const char *src,
				       const uint8_t *params, uint32_t plen,
				       const struct memory_api *mem,
				       uint32_t *out_size);

#endif /* MESH_SCRIPT_H */
