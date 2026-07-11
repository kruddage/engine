/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef TEXTURE_H
#define TEXTURE_H

#include <stddef.h>
#include <stdint.h>

/*
 * On-the-wire texture blob — the pixel twin of mesh_blob. A texture is a
 * width*height grid of RGBA8 texels, but the asset catalog delivers a single
 * opaque byte buffer through get_data(), so we pack the pixels behind this
 * header: the header, then height rows of width texels, each texel four bytes
 * (R, G, B, A) top-to-bottom, left-to-right. Native and WASM share endianness
 * (little), so the bytes travel verbatim and upload straight to an
 * RGBA8_UNORM GPU texture.
 */
#define TEXTURE_BLOB_MAGIC 0x52545854u /* 'TXTR' LE; sentinel, not a version */

/* The only pixel format the built-in generators emit. */
#define TEXTURE_FORMAT_RGBA8 0u

struct texture_blob {
	uint32_t magic;  /* TEXTURE_BLOB_MAGIC */
	uint32_t width;
	uint32_t height;
	uint32_t format; /* TEXTURE_FORMAT_*; 0 = RGBA8 (the only kind) */
};

/* Borrow the RGBA8 pixel array packed immediately after the header. */
static inline const uint8_t *texture_blob_pixels(const struct texture_blob *b)
{
	return (const uint8_t *)(b + 1);
}

/* Total byte size of a blob holding a width x height RGBA8 image. */
static inline uint32_t texture_blob_size(uint32_t width, uint32_t height)
{
	return (uint32_t)(sizeof(struct texture_blob)
			  + (size_t)width * (size_t)height * 4u);
}

#endif /* TEXTURE_H */
