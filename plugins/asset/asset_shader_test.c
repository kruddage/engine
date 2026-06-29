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

int main(void)
{
	mem_init();
	log_init();

	asset_init();

	check_shader(VERT_PATH, "vertex");
	check_shader(FRAG_PATH, "fragment");
	check_authored_roundtrip();

	log_shutdown();
	mem_shutdown();

	printf("asset_shader tests passed\n");
	return 0;
}
