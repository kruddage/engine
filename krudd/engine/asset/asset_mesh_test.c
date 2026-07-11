/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "asset.h"
#include "asset_api.h"
#include "log.h"
#include "memory.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

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

/*
 * A built-in mesh must be a loaded read-only ASSET_TYPE_MESH asset whose
 * get_data() returns the (mesh ...) source text verbatim — the asset stores
 * Scheme source, not a compiled mesh_blob (see seed_mesh in asset_plugin.c);
 * a consumer resolves it to a blob on demand through mesh_script_generate().
 */
static void check_mesh(const char *path)
{
	struct asset_info info;
	const char        *src;
	uint32_t           size;
	int32_t            idx;

	idx = index_of(path);
	assert(idx >= 0);

	assert(asset_catalog_info((uint32_t)idx, &info) == 0);
	assert(info.type      == ASSET_TYPE_MESH);
	assert(info.kind      == ASSET_KIND_PRIMITIVE);
	assert(info.read_only == 1);
	assert(info.state     == ASSET_LOADED);
	assert(info.id        != 0);

	size = 0;
	src = (const char *)asset_catalog_get_data(info.id, &size);
	assert(src != NULL);
	assert(size == strlen(src) + 1);
	assert(strncmp(src, "(mesh", 5) == 0);
}

int main(void)
{
	mem_init();
	log_init();

	asset_init();

	check_mesh("builtin://mesh/cube");
	check_mesh("builtin://mesh/box");
	check_mesh("builtin://mesh/sphere");
	check_mesh("builtin://mesh/plane");
	check_mesh("builtin://mesh/pyramid");
	check_mesh("builtin://mesh/grid");

	log_shutdown();
	mem_shutdown();

	printf("asset_mesh tests passed\n");
	return 0;
}
