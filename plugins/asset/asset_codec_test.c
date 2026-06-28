/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "asset.h"
#include "log.h"
#include "memory.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define TEST_PATH "/tmp/asset_codec_test_krudd.foo"
#define TEST_DATA "hello codec"

static void *dummy_decode(const void *bytes, uint32_t size)
{
	char *copy = mem_alloc(size + 1);

	assert(copy);
	memcpy(copy, bytes, size);
	copy[size] = '\0';
	return copy;
}

static void write_test_file(void)
{
	FILE *f = fopen(TEST_PATH, "wb");

	assert(f);
	fwrite(TEST_DATA, 1, strlen(TEST_DATA), f);
	fclose(f);
}

int main(void)
{
	char *result;

	mem_init();
	log_init();
	write_test_file();

	asset_request(TEST_PATH);
	assert(asset_state_of(TEST_PATH) == ASSET_LOADED);

	/* No decoder registered yet — get_typed returns NULL. */
	assert(asset_codec_get_typed(TEST_PATH) == NULL);

	/* Register a decoder for the "foo" extension. */
	asset_codec_register("foo", dummy_decode);

	/* Verify get_typed invokes the decoder and returns expected output. */
	result = asset_codec_get_typed(TEST_PATH);
	assert(result != NULL);
	assert(strcmp(result, TEST_DATA) == 0);
	mem_free(result);

	/* Path with no extension returns NULL. */
	assert(asset_codec_get_typed("/tmp/no_ext") == NULL);

	/* Extension with no registered decoder returns NULL. */
	assert(asset_codec_get_typed("/tmp/asset_codec_test_krudd.bar") == NULL);

	asset_release(TEST_PATH);

	log_shutdown();
	mem_shutdown();

	printf("asset_codec tests passed\n");
	return 0;
}
