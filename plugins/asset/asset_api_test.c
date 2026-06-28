/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "asset.h"
#include "asset_api.h"
#include "log.h"
#include "memory.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define TEST_PATH "/tmp/asset_api_test_krudd.def"
#define TEST_DATA "test"

static void write_test_file(void)
{
	FILE *f = fopen(TEST_PATH, "wb");

	assert(f);
	fwrite(TEST_DATA, 1, strlen(TEST_DATA), f);
	fclose(f);
}

int main(void)
{
	uint32_t         n;
	uint32_t         i;
	struct asset_info info;
	int32_t          found_cube = 0;
	int32_t          found_sphere = 0;
	int32_t          found_plane = 0;
	int32_t          found_pyramid = 0;

	mem_init();
	log_init();

	/* Initialise the asset subsystem — seeds built-in primitives. */
	asset_init();

	/* Catalog must be non-empty immediately after init. */
	n = asset_catalog_count();
	assert(n >= 4);

	/* All built-ins must be present, loaded, read-only, and PRIMITIVE. */
	for (i = 0; i < n; i++) {
		assert(asset_catalog_info(i, &info) == 0);
		if (info.kind != ASSET_KIND_PRIMITIVE)
			continue;
		assert(info.read_only == 1);
		assert(info.state == ASSET_LOADED);
		if (strcmp(info.path, "builtin://cube")    == 0) found_cube    = 1;
		if (strcmp(info.path, "builtin://sphere")  == 0) found_sphere  = 1;
		if (strcmp(info.path, "builtin://plane")   == 0) found_plane   = 1;
		if (strcmp(info.path, "builtin://pyramid") == 0) found_pyramid = 1;
	}
	assert(found_cube);
	assert(found_sphere);
	assert(found_plane);
	assert(found_pyramid);

	/* out-of-range index returns -1 */
	assert(asset_catalog_info(n, &info) == -1);
	/* NULL out-pointer returns -1 */
	assert(asset_catalog_info(0, NULL) == -1);

	/* Built-ins are not evicted when asset_release is called on them. */
	asset_request("builtin://cube");
	assert(asset_state_of("builtin://cube") == ASSET_LOADED);
	asset_release("builtin://cube");
	assert(asset_state_of("builtin://cube") == ASSET_LOADED);
	assert(asset_catalog_count() == n); /* count unchanged */

	/* Normal project assets still work alongside built-ins. */
	write_test_file();
	asset_request(TEST_PATH);
	assert(asset_state_of(TEST_PATH) == ASSET_LOADED);
	assert(asset_catalog_count() == n + 1);

	asset_release(TEST_PATH);
	assert(asset_state_of(TEST_PATH) == ASSET_ERROR);
	assert(asset_catalog_count() == n); /* project asset evicted */

	/* Built-ins still present after project-asset eviction. */
	for (i = 0; i < asset_catalog_count(); i++) {
		assert(asset_catalog_info(i, &info) == 0);
		if (strcmp(info.path, "builtin://cube") == 0) {
			assert(info.kind      == ASSET_KIND_PRIMITIVE);
			assert(info.read_only == 1);
			assert(info.state     == ASSET_LOADED);
		}
	}

	log_shutdown();
	mem_shutdown();

	printf("asset_api tests passed\n");
	return 0;
}
