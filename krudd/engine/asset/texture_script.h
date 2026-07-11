/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef TEXTURE_SCRIPT_H
#define TEXTURE_SCRIPT_H

#include "texture.h"
#include "memory_api.h"

#include <stdint.h>

/*
 * texture_script — the bridge between an ASSET_TYPE_TEXTURE asset's Scheme
 * source and a real texture_blob. There is no other kind of texture asset:
 * every built-in or authored texture is a (texture NAME (shade (u v) ...))
 * form, and this is what bakes one into GPU-uploadable pixels.
 *
 * Evaluate SRC — a (texture NAME [(params ...)] (shade (u v) ...)) form, see
 * core/texture_script.scm — against the shared s7 image, sampling its shade
 * clause over a WIDTH x HEIGHT grid, and marshal the RGBA8 result into a
 * heap-allocated texture_blob. WIDTH/HEIGHT are the output resolution the
 * caller chooses (the script is resolution-independent); each is clamped to
 * TEXTURE_SCRIPT_MAX_DIM. PARAMS (PLEN bytes, or NULL) is the tight-packed
 * override the shade body reads through (param ...); missing/short params fall
 * back to their declared defaults.
 *
 * The caller owns the returned buffer and frees it with mem->free; *out_size
 * (may be NULL) receives the total byte count. Returns NULL when the
 * interpreter is unavailable, WIDTH/HEIGHT are zero, SRC fails to parse/eval, or
 * its shade clause faults — never a partially filled blob.
 */
#define TEXTURE_SCRIPT_MAX_DIM 4096u

struct texture_blob *texture_script_generate(const char *src,
					     const uint8_t *params,
					     uint32_t plen,
					     uint32_t width, uint32_t height,
					     const struct memory_api *mem,
					     uint32_t *out_size);

#endif /* TEXTURE_SCRIPT_H */
