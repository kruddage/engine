/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef TEXTURE_H
#define TEXTURE_H

#include <stddef.h>
#include <stdint.h>

/*
 * On-the-wire texture blob. A texture is a 2D image, but the asset catalog
 * delivers a single opaque byte buffer, so we pack the dimensions and the
 * pixels behind this header: the header, then width * height texels of
 * RGBA8 (4 bytes each), row-major from the top-left. Native and WASM share
 * endianness (little), so the bytes travel verbatim — the mesh_blob story
 * for geometry, now for pixels.
 *
 * A texture is generated from a (texture NAME (shade (u v) ...)) Scheme
 * script (see core/texture_script.scm); the generator is a pure function of
 * the normalized coordinate u,v in [0,1), so the same script bakes at any
 * resolution. width/height are an output setting the baker supplies, never
 * baked into the script.
 */
#define TEXTURE_BLOB_MAGIC 0x30584554u /* 'TEX0' sentinel, not a version */

/* format discriminator; 0 = RGBA8_UNORM (the only kind today). */
#define TEXTURE_FORMAT_RGBA8 0u

struct texture_blob {
	uint32_t magic;     /* TEXTURE_BLOB_MAGIC */
	uint32_t width;
	uint32_t height;
	uint32_t format;    /* texture_format; 0 = RGBA8 */
	uint32_t mip_count; /* levels stored after the header; v0 always 1 (base) */
};

/* Bytes one RGBA8 texel occupies. */
#define TEXTURE_TEXEL_BYTES 4u

/* Borrow the base-level (mip 0) pixel array packed after the header. */
static inline const uint8_t *texture_blob_pixels(const struct texture_blob *b)
{
	return (const uint8_t *)(b + 1);
}

/* Byte size of the base-level pixel array for the given dimensions. */
static inline uint32_t texture_blob_pixels_size(uint32_t width, uint32_t height)
{
	return (uint32_t)((size_t)width * height * TEXTURE_TEXEL_BYTES);
}

/* Total byte size of a blob holding one RGBA8 level of the given dimensions. */
static inline uint32_t texture_blob_size(uint32_t width, uint32_t height)
{
	return (uint32_t)(sizeof(struct texture_blob)
			  + texture_blob_pixels_size(width, height));
}

#endif /* TEXTURE_H */
