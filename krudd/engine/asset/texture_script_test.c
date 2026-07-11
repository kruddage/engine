/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * texture_script — the texture scripting path end to end: boot the real s7
 * image (which loads the embedded texture_script.scm), call
 * texture_script_generate() against a hand-authored (texture ...) source at
 * two resolutions, and check the returned texture_blob's header and pixels.
 * The load-bearing property is resolution independence: the same script bakes
 * the same normalized pattern at 8x8 and 32x32, because shade is a pure
 * function of u,v in [0,1) and never sees a size.
 */
#include "texture_script.h"

#include "script.h"
#include "log.h"
#include "memory.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define EPS 1.0e-5f

static const struct memory_api test_mem_impl = {
	mem_alloc, mem_alloc_zero, mem_free,
	mem_pool_create, mem_pool_alloc, mem_pool_free, mem_pool_destroy,
};
static const struct memory_api *g_mem = &test_mem_impl;

/*
 * A 2x2 checker, the texture twin of mesh_script_test's hand-authored quad: a
 * literal shade body reading its one param through (param ...). scale=2 splits
 * uv space into quadrants — top-left white, bottom-left dark — so a fixed
 * normalized coordinate lands on the same colour at any resolution.
 */
static const char *CHECKER_SRC =
	"(texture checker\n"
	"  (params (scale float (edit range 1 64) (default 2)))\n"
	"  (shade (u v)\n"
	"    (let ((s (or (param 'scale) 2.0)))\n"
	"      (if (even? (+ (tex-ifloor (* u s)) (tex-ifloor (* v s))))\n"
	"          (list 1.0 1.0 1.0 1.0)\n"
	"          (list 0.1 0.1 0.1 1.0)))))\n";

/* The RGBA8 texel at (x, y), row-major from the top-left. */
static const uint8_t *texel(const struct texture_blob *b, uint32_t x, uint32_t y)
{
	return texture_blob_pixels(b) + (size_t)(y * b->width + x) * 4u;
}

/* 0.1 quantized round-to-nearest: floor(0.5 + 255*0.1) = floor(26.0) = 26. */
#define DARK8 26u

static void test_hand_authored_checker(void)
{
	struct texture_blob *b;
	const uint8_t       *tl, *bl;
	uint32_t             size = 0;

	b = texture_script_generate(CHECKER_SRC, NULL, 0, 8, 8, g_mem, &size);
	assert(b != NULL);
	assert(size == texture_blob_size(8, 8));
	assert(b->magic     == TEXTURE_BLOB_MAGIC);
	assert(b->width     == 8 && b->height == 8);
	assert(b->format    == TEXTURE_FORMAT_RGBA8);
	assert(b->mip_count == 1);

	/* Top-left quadrant is white, bottom-left is dark, alpha opaque. */
	tl = texel(b, 0, 0);
	assert(tl[0] == 255 && tl[1] == 255 && tl[2] == 255 && tl[3] == 255);
	bl = texel(b, 0, 7);
	assert(bl[0] == DARK8 && bl[1] == DARK8 && bl[2] == DARK8 && bl[3] == 255);

	g_mem->free(b);
}

/*
 * The heart of resolution independence: one source, two sizes, one pattern.
 * The pixel count scales with area, but the normalized quadrants land on the
 * same colours — top-left white and bottom-left dark at both 8x8 and 32x32.
 */
static void test_resolution_independence(void)
{
	struct texture_blob *small, *big;
	uint32_t             ssz = 0, bsz = 0;

	small = texture_script_generate(CHECKER_SRC, NULL, 0, 8, 8, g_mem, &ssz);
	big   = texture_script_generate(CHECKER_SRC, NULL, 0, 32, 32, g_mem, &bsz);
	assert(small != NULL && big != NULL);
	assert(ssz == texture_blob_size(8, 8));
	assert(bsz == texture_blob_size(32, 32));

	/* Same normalized corners, same colours, despite 16x the texels. */
	assert(texel(small, 0, 0)[0] == 255 && texel(big, 0, 0)[0] == 255);
	assert(texel(small, 0, 7)[0] == DARK8 && texel(big, 0, 31)[0] == DARK8);

	g_mem->free(small);
	g_mem->free(big);
}

/*
 * The checker declares a (params (scale ...)) clause. It must report one
 * tight-packed float field defaulting to 2.0 with a range edit hint — the same
 * shape script_mesh_params reports, so one marshaller and one set of editor
 * widgets serve textures too.
 */
static void test_checker_params_declared(void)
{
	struct shader_param p[8];
	uint32_t            total = 0;
	int                 n;

	n = script_texture_params(CHECKER_SRC, p, 8, &total);
	assert(n == 1);
	assert(total == sizeof(float)); /* tight-packed */
	assert(strcmp(p[0].name, "scale") == 0);
	assert(p[0].components == 1 && p[0].offset == 0);
	assert(strcmp(p[0].edit, "range") == 0);
	assert(fabsf(p[0].edit_min - 1.0f) <= EPS);
	assert(fabsf(p[0].edit_max - 64.0f) <= EPS);
	assert(p[0].default_count == 1 &&
	       fabsf(p[0].edit_default[0] - 2.0f) <= EPS);
}

/*
 * One texture source, a scale override, a different pattern. scale=1 makes
 * floor(u)=floor(v)=0 everywhere, so the whole texture is one (even) check —
 * white — turning the bottom-left texel from dark to white. Proof the shade
 * body reads (param ...) and the host resolves the override into it.
 */
static void test_param_override_changes_output(void)
{
	const float          one[1] = { 1.0f };
	struct texture_blob *b;

	b = texture_script_generate(CHECKER_SRC, (const uint8_t *)one,
				    sizeof(one), 8, 8, g_mem, NULL);
	assert(b != NULL);
	/* Bottom-left was dark at the default scale=2; scale=1 makes it white. */
	assert(texel(b, 0, 7)[0] == 255);
	g_mem->free(b);
}

/*
 * A source that isn't a well-formed (texture ...) form, or whose shade body
 * faults, must not crash the caller — texture-script-generate's catch degrades
 * to #f, which the host marshals to "no texture" (NULL), never a partial blob.
 * A zero dimension is likewise NULL, not a zero-byte allocation.
 */
static void test_malformed_source_yields_no_texture(void)
{
	assert(texture_script_generate("(not-a-texture-form)", NULL, 0,
				       8, 8, g_mem, NULL) == NULL);
	assert(texture_script_generate(
		       "(texture broken (shade (u v) (car '())))",
		       NULL, 0, 8, 8, g_mem, NULL) == NULL);
	assert(texture_script_generate(NULL, NULL, 0, 8, 8, g_mem, NULL) == NULL);
	assert(texture_script_generate(CHECKER_SRC, NULL, 0, 0, 8, g_mem, NULL)
	       == NULL);
}

int main(void)
{
	mem_init();
	log_init();
	script_init(); /* loads the embedded texture_script.scm image */

	test_hand_authored_checker();
	test_resolution_independence();
	test_checker_params_declared();
	test_param_override_changes_output();
	test_malformed_source_yields_no_texture();

	printf("texture_script tests passed\n");
	return 0;
}
