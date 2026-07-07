/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "asset.h"
#include "log.h"
#include "memory.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define TEST_PATH "/tmp/asset_test_krudd.def"
#define TEST_DATA "{\"type\":\"item\",\"name\":\"sword\"}"

static void write_test_file(void)
{
	FILE *f = fopen(TEST_PATH, "wb");

	assert(f);
	fwrite(TEST_DATA, 1, strlen(TEST_DATA), f);
	fclose(f);
}

int main(void)
{
	const char *data;
	uint32_t    size;

	mem_init();
	log_init();
	write_test_file();

	/* Basic load — native path is synchronous */
	asset_request(TEST_PATH);
	assert(asset_state_of(TEST_PATH) == ASSET_LOADED);
	data = asset_get(TEST_PATH, &size);
	assert(data != NULL);
	assert(size == (uint32_t)strlen(TEST_DATA));
	assert(memcmp(data, TEST_DATA, size) == 0);

	/* get with NULL size out-param */
	assert(asset_get(TEST_PATH, NULL) != NULL);

	/* Dedup: second request bumps ref count, same data */
	asset_request(TEST_PATH);
	assert(asset_state_of(TEST_PATH) == ASSET_LOADED);

	/* Release one ref — still loaded */
	asset_release(TEST_PATH);
	assert(asset_state_of(TEST_PATH) == ASSET_LOADED);

	/* Release last ref — evicted */
	asset_release(TEST_PATH);
	assert(asset_state_of(TEST_PATH) == ASSET_ERROR);
	assert(asset_get(TEST_PATH, NULL) == NULL);

	/* Error case: nonexistent file */
	asset_request("/tmp/asset_test_krudd_missing.def");
	assert(asset_state_of("/tmp/asset_test_krudd_missing.def") == ASSET_ERROR);
	asset_release("/tmp/asset_test_krudd_missing.def");

	log_shutdown();
	mem_shutdown();

	printf("asset tests passed\n");
	return 0;
}
