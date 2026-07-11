/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "textures.h"
#include "texture.h"

#include <stdint.h>

/*
 * Procedural pixels for the engine's built-in texture library — the twin of
 * primitives.c. Each generator fills a texture_blob in place (never a large
 * stack array — a 64x64 RGBA image is 16 KB) and hands back an owned buffer.
 * Every texel is arithmetic over its (x, y) coordinate: no source images, no
 * dependencies. Dimensions mirror the describe() metadata in asset_plugin.c so
 * the two stay in agreement.
 */

#define TEX_SIZE TEXTURE_BUILTIN_SIZE

/* Writable view of the pixels (texture_blob_pixels hands out a const view). */
static uint8_t *blob_pixels(struct texture_blob *b)
{
	return (uint8_t *)(b + 1);
}

static struct texture_blob *blob_alloc(const struct memory_api *mem,
				       uint32_t w, uint32_t h,
				       uint32_t *out_size)
{
	struct texture_blob *b;
	uint32_t             size;

	size = texture_blob_size(w, h);
	b = mem->alloc(size);
	if (!b)
		return NULL;
	b->magic  = TEXTURE_BLOB_MAGIC;
	b->width  = w;
	b->height = h;
	b->format = TEXTURE_FORMAT_RGBA8;
	if (out_size)
		*out_size = size;
	return b;
}

/* Store one opaque RGBA texel at (x, y) in a row-major w-wide image. */
static void put(uint8_t *px, uint32_t w, uint32_t x, uint32_t y,
		uint8_t r, uint8_t g, uint8_t b)
{
	uint8_t *t = px + ((size_t)y * w + x) * 4u;

	t[0] = r;
	t[1] = g;
	t[2] = b;
	t[3] = 255;
}

/* --- Checker: two-colour board, the universal UV sanity texture ---------- */

#define CHECKER_CELLS 8u /* squares per edge; TEX_SIZE must divide evenly */

static struct texture_blob *gen_checker(const struct memory_api *mem,
					uint32_t *out_size)
{
	const uint32_t       cell = TEX_SIZE / CHECKER_CELLS;
	struct texture_blob *b;
	uint8_t             *px;
	uint32_t             x, y;

	b = blob_alloc(mem, TEX_SIZE, TEX_SIZE, out_size);
	if (!b)
		return NULL;
	px = blob_pixels(b);

	for (y = 0; y < TEX_SIZE; y++) {
		for (x = 0; x < TEX_SIZE; x++) {
			if (((x / cell) ^ (y / cell)) & 1u)
				put(px, TEX_SIZE, x, y, 60, 60, 70);
			else
				put(px, TEX_SIZE, x, y, 200, 200, 210);
		}
	}
	return b;
}

/* --- Grid: thin lines on a solid ground, for scale / UV debugging -------- */

#define GRID_STEP 16u /* line spacing in texels */

static struct texture_blob *gen_grid(const struct memory_api *mem,
				     uint32_t *out_size)
{
	struct texture_blob *b;
	uint8_t             *px;
	uint32_t             x, y;

	b = blob_alloc(mem, TEX_SIZE, TEX_SIZE, out_size);
	if (!b)
		return NULL;
	px = blob_pixels(b);

	for (y = 0; y < TEX_SIZE; y++) {
		for (x = 0; x < TEX_SIZE; x++) {
			if (x % GRID_STEP == 0 || y % GRID_STEP == 0)
				put(px, TEX_SIZE, x, y, 120, 130, 150);
			else
				put(px, TEX_SIZE, x, y, 40, 44, 52);
		}
	}
	return b;
}

/* --- UV: rgb = (u, v, 0), a direct read-out of texture coordinates -------- */

static struct texture_blob *gen_uv(const struct memory_api *mem,
				   uint32_t *out_size)
{
	struct texture_blob *b;
	uint8_t             *px;
	uint32_t             x, y;

	b = blob_alloc(mem, TEX_SIZE, TEX_SIZE, out_size);
	if (!b)
		return NULL;
	px = blob_pixels(b);

	for (y = 0; y < TEX_SIZE; y++) {
		for (x = 0; x < TEX_SIZE; x++) {
			uint8_t u = (uint8_t)(x * 255u / (TEX_SIZE - 1u));
			uint8_t v = (uint8_t)(y * 255u / (TEX_SIZE - 1u));

			put(px, TEX_SIZE, x, y, u, v, 0);
		}
	}
	return b;
}

/* --- Noise: value-noise fBm, the gateway to clouds / marble / terrain ---- */

/* Integer bit-mix (an fmix-style avalanche hash) — deterministic, no state. */
static uint32_t hash2(int32_t xi, int32_t yi)
{
	uint32_t h = (uint32_t)xi * 374761393u + (uint32_t)yi * 668265263u;

	h ^= h >> 13;
	h *= 0x5bd1e995u;
	h ^= h >> 15;
	return h;
}

/* Pseudo-random value in [0, 1] at integer lattice point (xi, yi). */
static float lattice(int32_t xi, int32_t yi)
{
	return (float)(hash2(xi, yi) & 0xffffffu) / (float)0xffffffu;
}

static float lerpf(float a, float b, float t)
{
	return a + (b - a) * t;
}

/* Smoothstep easing so the bilinear blend has no lattice creases. */
static float fade(float t)
{
	return t * t * (3.0f - 2.0f * t);
}

/* Bilinearly interpolated value noise at continuous (x, y). */
static float value_noise(float x, float y)
{
	int32_t xi = (int32_t)x;
	int32_t yi = (int32_t)y;
	float   tx = fade(x - (float)xi);
	float   ty = fade(y - (float)yi);
	float   top = lerpf(lattice(xi, yi), lattice(xi + 1, yi), tx);
	float   bot = lerpf(lattice(xi, yi + 1), lattice(xi + 1, yi + 1), tx);

	return lerpf(top, bot, ty);
}

/* Fractal Brownian motion: octaves of value noise at halving amplitude. */
static float fbm(float x, float y)
{
	float sum  = 0.0f;
	float amp  = 0.5f;
	float freq = 1.0f;
	int   o;

	for (o = 0; o < 5; o++) {
		sum  += amp * value_noise(x * freq, y * freq);
		freq *= 2.0f;
		amp  *= 0.5f;
	}
	return sum;
}

static struct texture_blob *gen_noise(const struct memory_api *mem,
				      uint32_t *out_size)
{
	/* Base lattice ~4 cells across the texture; fBm adds the fine detail. */
	const float          scale = 4.0f / (float)TEX_SIZE;
	struct texture_blob *b;
	uint8_t             *px;
	uint32_t             x, y;

	b = blob_alloc(mem, TEX_SIZE, TEX_SIZE, out_size);
	if (!b)
		return NULL;
	px = blob_pixels(b);

	for (y = 0; y < TEX_SIZE; y++) {
		for (x = 0; x < TEX_SIZE; x++) {
			float   n = fbm((float)x * scale, (float)y * scale);
			uint8_t g;

			if (n < 0.0f)
				n = 0.0f;
			if (n > 1.0f)
				n = 1.0f;
			g = (uint8_t)(n * 255.0f);
			put(px, TEX_SIZE, x, y, g, g, g);
		}
	}
	return b;
}

void *texture_generate(enum texture_kind kind,
		       const struct memory_api *mem, uint32_t *out_size)
{
	if (!mem)
		return NULL;

	switch (kind) {
	case TEXTURE_CHECKER: return gen_checker(mem, out_size);
	case TEXTURE_GRID:    return gen_grid(mem, out_size);
	case TEXTURE_UV:      return gen_uv(mem, out_size);
	case TEXTURE_NOISE:   return gen_noise(mem, out_size);
	}
	return NULL;
}
