/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Invariants every built-in texture generator must satisfy: exact blob
 * structure (magic, dimensions, format, byte size), fully opaque coverage, and
 * the defining property of each pattern (checker is two-valued, grid draws its
 * lines, uv reads back its coordinates, noise stays in range and is
 * deterministic). The same-spirit companion to primitive_test.c.
 */
#include "textures.h"
#include "texture.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* A malloc-backed memory_api: the generators only use alloc and free. */
static const struct memory_api test_mem = {
	.alloc = malloc,
	.free  = free,
};

static const struct texture_blob *gen(enum texture_kind kind, uint32_t *size)
{
	const struct texture_blob *b = texture_generate(kind, &test_mem, size);

	assert(b != NULL);
	assert(*size == texture_blob_size(TEXTURE_BUILTIN_SIZE,
					  TEXTURE_BUILTIN_SIZE));
	assert(b->magic  == TEXTURE_BLOB_MAGIC);
	assert(b->width  == TEXTURE_BUILTIN_SIZE);
	assert(b->height == TEXTURE_BUILTIN_SIZE);
	assert(b->format == TEXTURE_FORMAT_RGBA8);
	return b;
}

/* Every texel is fully opaque — the built-ins carry no transparency. */
static void check_opaque(const struct texture_blob *b)
{
	const uint8_t *px = texture_blob_pixels(b);
	uint32_t       i, n = b->width * b->height;

	for (i = 0; i < n; i++)
		assert(px[i * 4u + 3u] == 255);
}

static const uint8_t *texel(const struct texture_blob *b, uint32_t x, uint32_t y)
{
	return texture_blob_pixels(b) + ((size_t)y * b->width + x) * 4u;
}

int main(void)
{
	uint32_t                   size;
	const struct texture_blob *b;

	/* checker: exactly two distinct colours, and adjacent cells differ. */
	{
		const uint32_t cell = TEXTURE_BUILTIN_SIZE / 8u;
		const uint8_t *a, *c;

		b = gen(TEXTURE_CHECKER, &size);
		check_opaque(b);
		a = texel(b, 0, 0);
		c = texel(b, cell, 0);
		assert(a[0] != c[0] || a[1] != c[1] || a[2] != c[2]);
		/* Two cells over is the same colour again. */
		c = texel(b, cell * 2u, 0);
		assert(a[0] == c[0] && a[1] == c[1] && a[2] == c[2]);
		free((void *)b);
	}

	/* grid: pixels on a 16-step line differ from an off-line pixel. */
	{
		const uint8_t *line, *ground;

		b = gen(TEXTURE_GRID, &size);
		check_opaque(b);
		line   = texel(b, 16, 16);
		ground = texel(b, 8, 8);
		assert(line[0] != ground[0] || line[1] != ground[1]
		       || line[2] != ground[2]);
		free((void *)b);
	}

	/* uv: red rises with x, green with y, blue is flat zero. */
	{
		b = gen(TEXTURE_UV, &size);
		check_opaque(b);
		assert(texel(b, 0, 0)[0] == 0);
		assert(texel(b, TEXTURE_BUILTIN_SIZE - 1u, 0)[0] == 255);
		assert(texel(b, 0, TEXTURE_BUILTIN_SIZE - 1u)[1] == 255);
		assert(texel(b, TEXTURE_BUILTIN_SIZE - 1u, 0)[1] == 0);
		assert(texel(b, 0, 0)[2] == 0);
		free((void *)b);
	}

	/* noise: grayscale, in range, and deterministic across two runs. */
	{
		uint32_t                   size2;
		const struct texture_blob *b2;
		const uint8_t             *p, *q;
		uint32_t                   i, n;

		b  = gen(TEXTURE_NOISE, &size);
		b2 = gen(TEXTURE_NOISE, &size2);
		check_opaque(b);
		p = texture_blob_pixels(b);
		q = texture_blob_pixels(b2);
		n = b->width * b->height;
		for (i = 0; i < n; i++) {
			assert(p[i * 4u] == p[i * 4u + 1u]);   /* r == g */
			assert(p[i * 4u] == p[i * 4u + 2u]);   /* r == b */
			assert(p[i * 4u] == q[i * 4u]);        /* reproducible */
		}
		free((void *)b);
		free((void *)b2);
	}

	/* An out-of-range kind yields no texture. */
	assert(texture_generate((enum texture_kind)99, &test_mem, NULL) == NULL);

	printf("texture tests passed\n");
	return 0;
}
