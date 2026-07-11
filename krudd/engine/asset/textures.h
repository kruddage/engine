/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef TEXTURES_H
#define TEXTURES_H

#include "texture.h"
#include "memory_api.h"

#include <stdint.h>

/*
 * Which built-in texture to generate. The order mirrors the texture_paths
 * table in asset_plugin.c so a catalog entry maps straight to a kind.
 */
enum texture_kind {
	TEXTURE_CHECKER = 0,
	TEXTURE_GRID,
	TEXTURE_UV,
	TEXTURE_NOISE,
};

/* Edge length of every built-in texture, in texels (square, power of two). */
#define TEXTURE_BUILTIN_SIZE 64u

/*
 * Generate a texture blob (struct texture_blob header + row-major RGBA8 pixels)
 * for the given kind, allocated through mem. The caller owns the buffer and
 * frees it with mem->free; *out_size receives the total byte count. Returns
 * NULL on a bad kind or allocation failure. Every built-in is a square
 * TEXTURE_BUILTIN_SIZE x TEXTURE_BUILTIN_SIZE opaque RGBA8 image.
 */
void *texture_generate(enum texture_kind kind,
		       const struct memory_api *mem, uint32_t *out_size);

#endif /* TEXTURES_H */
