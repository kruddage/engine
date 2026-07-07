/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "asset.h"
#include "asset_api.h"
#include "log.h"
#include "memory.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define VERT_PATH "builtin://shader/triangle.vert"
#define FRAG_PATH "builtin://shader/triangle.frag"

/* Locate a built-in by path; return its catalog index, or -1 if absent. */
static int32_t index_of(const char *path)
{
	struct asset_info info;
	uint32_t          i;
	uint32_t          n;

	n = asset_catalog_count();
	for (i = 0; i < n; i++) {
		if (asset_catalog_info(i, &info) != 0)
			continue;
		if (strcmp(info.path, path) == 0)
			return (int32_t)i;
	}
	return -1;
}

/* Look up a decl field's value by key for catalog entry i. */
static const char *decl_value(uint32_t i, const char *key)
{
	struct asset_decl_field fields[8];
	uint32_t                n;
	uint32_t                f;

	n = asset_catalog_describe(i, fields, 8);
	for (f = 0; f < n; f++) {
		if (strcmp(fields[f].key, key) == 0)
			return fields[f].value;
	}
	return NULL;
}

/*
 * A built-in shader asset must exist, be a loaded read-only primitive of type
 * SHADER, deliver NUL-terminated GLSL source through get_data(), and carry the
 * expected dialect/stage metadata via describe().
 */
static void check_shader(const char *path, const char *stage)
{
	struct asset_info info;
	const char       *src;
	uint32_t          size;
	int32_t           idx;

	idx = index_of(path);
	assert(idx >= 0);

	assert(asset_catalog_info((uint32_t)idx, &info) == 0);
	assert(info.type      == ASSET_TYPE_SHADER);
	assert(info.kind      == ASSET_KIND_PRIMITIVE);
	assert(info.read_only == 1);
	assert(info.state     == ASSET_LOADED);
	assert(info.id        != 0);

	/* Raw source round-trips through the asset system, NUL-terminated. */
	size = 0;
	src  = asset_catalog_get_data(info.id, &size);
	assert(src != NULL);
	assert(size > 0);
	assert(src[size - 1] == '\0');
	assert(strlen(src) + 1 == size);
	assert(strstr(src, "#version 300 es") == src);

	/* Dialect + stage metadata rides on the asset. */
	assert(decl_value((uint32_t)idx, "dialect") != NULL);
	assert(strcmp(decl_value((uint32_t)idx, "dialect"), "glsl_es_300") == 0);
	assert(decl_value((uint32_t)idx, "stage") != NULL);
	assert(strcmp(decl_value((uint32_t)idx, "stage"), stage) == 0);
}

/*
 * Authored shaders round-trip arbitrary source through the same path: the
 * asset system is dialect-agnostic storage; the renderer owns compilation.
 */
static void check_authored_roundtrip(void)
{
	static const char  src[] = "#version 300 es\nvoid main() {}\n";
	const char        *out;
	uint32_t           size;
	uint32_t           id;

	id = asset_mut_create("project://shader/custom.frag",
			      ASSET_TYPE_SHADER, src, (uint32_t)sizeof(src));
	assert(id != 0);

	size = 0;
	out  = asset_catalog_get_data(id, &size);
	assert(out != NULL);
	assert(size == (uint32_t)sizeof(src));
	assert(memcmp(out, src, sizeof(src)) == 0);

	assert(asset_mut_destroy(id) == 0);
}

/*
 * Authored shader declarations: with no explicit decl, describe() synthesizes
 * stage from the file extension and a default dialect; set_decl overrides it;
 * and it is rejected on read-only assets, on over-capacity counts, and cleared
 * by n == 0 (which falls back to the synthesized values).
 */
static void check_authored_decl(void)
{
	struct asset_decl_field one[1];
	struct asset_decl_field two[2];
	struct asset_info       info;
	uint32_t                frag;
	uint32_t                vert;
	int32_t                 idx;

	/* No explicit decl: stage derives from the extension. */
	frag = asset_mut_create("project://shader/ripple.frag",
				ASSET_TYPE_SHADER, "", 0);
	vert = asset_mut_create("project://shader/wave.vert",
				ASSET_TYPE_SHADER, "", 0);
	assert(frag != 0 && vert != 0);

	idx = index_of("project://shader/ripple.frag");
	assert(idx >= 0);
	assert(strcmp(decl_value((uint32_t)idx, "dialect"), "glsl_es_300") == 0);
	assert(strcmp(decl_value((uint32_t)idx, "stage"), "fragment") == 0);

	idx = index_of("project://shader/wave.vert");
	assert(idx >= 0);
	assert(strcmp(decl_value((uint32_t)idx, "stage"), "vertex") == 0);

	/* set_decl overrides the synthesized values. */
	two[0].key = "dialect"; two[0].value = "glsl_es_300";
	two[1].key = "stage";   two[1].value = "vertex";
	assert(asset_mut_set_decl(frag, two, 2) == 0);
	idx = index_of("project://shader/ripple.frag");
	assert(strcmp(decl_value((uint32_t)idx, "stage"), "vertex") == 0);

	/* n == 0 clears the decl; describe() falls back to the extension. */
	assert(asset_mut_set_decl(frag, NULL, 0) == 0);
	idx = index_of("project://shader/ripple.frag");
	assert(strcmp(decl_value((uint32_t)idx, "stage"), "fragment") == 0);

	/* An over-capacity count is rejected (max is small and internal). */
	assert(asset_mut_set_decl(frag, one, 1000) == -1);

	/* null fields with n > 0 is rejected. */
	assert(asset_mut_set_decl(frag, NULL, 1) == -1);

	/* set_decl on a read-only built-in is rejected. */
	assert(asset_catalog_info(0, &info) == 0);
	assert(info.read_only == 1);
	one[0].key = "stage"; one[0].value = "vertex";
	assert(asset_mut_set_decl(info.id, one, 1) == -1);

	assert(asset_mut_destroy(frag) == 0);
	assert(asset_mut_destroy(vert) == 0);
}

int main(void)
{
	mem_init();
	log_init();

	asset_init();

	check_shader(VERT_PATH, "vertex");
	check_shader(FRAG_PATH, "fragment");
	check_authored_roundtrip();
	check_authored_decl();

	log_shutdown();
	mem_shutdown();

	printf("asset_shader tests passed\n");
	return 0;
}
