/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "asset.h"
#include "asset_api.h"
#include "log.h"
#include "memory.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define TEXT_PATH  "authored://hello.md"
#define TEXT_BYTES "# Hello\nWorld\n"
#define TEXT_SIZE  ((uint32_t)(sizeof(TEXT_BYTES) - 1))

#define TEXT2_BYTES "# Updated\n"
#define TEXT2_SIZE  ((uint32_t)(sizeof(TEXT2_BYTES) - 1))

int main(void)
{
	uint32_t          base_count;
	uint32_t          id;
	uint32_t          id2;
	struct asset_info info;
	const void       *data;
	uint32_t          sz;
	int32_t           i;
	int32_t           found;

	mem_init();
	log_init();
	asset_init();

	base_count = asset_catalog_count();

	/* -------------------------------------------------------------- */
	/* create: born-loaded authored TEXT asset                         */
	/* -------------------------------------------------------------- */
	id = asset_mut_create(TEXT_PATH, ASSET_TYPE_TEXT,
			      TEXT_BYTES, TEXT_SIZE);
	assert(id != 0);
	assert(asset_catalog_count() == base_count + 1);

	/* appears in catalog as LOADED / NORMAL / authored */
	found = 0;
	for (i = 0; i < (int32_t)asset_catalog_count(); i++) {
		assert(asset_catalog_info((uint32_t)i, &info) == 0);
		if (strcmp(info.path, TEXT_PATH) != 0)
			continue;
		assert(info.state     == ASSET_LOADED);
		assert(info.kind      == ASSET_KIND_NORMAL);
		assert(info.read_only == 0);
		assert(info.origin    == ASSET_ORIGIN_AUTHORED);
		assert(info.type      == ASSET_TYPE_TEXT);
		assert(info.size      == TEXT_SIZE);
		assert(info.id        == id);
		found = 1;
		break;
	}
	assert(found);

	/* find() resolves the stable id */
	assert(asset_catalog_find(id, &info) == 0);
	assert(info.id     == id);
	assert(info.origin == ASSET_ORIGIN_AUTHORED);
	assert(info.type   == ASSET_TYPE_TEXT);

	/* -------------------------------------------------------------- */
	/* set_data: replace bytes in place, verify via asset_get         */
	/* -------------------------------------------------------------- */
	assert(asset_mut_set_data(id, TEXT2_BYTES, TEXT2_SIZE) == 0);
	data = asset_get(TEXT_PATH, &sz);
	assert(data != NULL);
	assert(sz == TEXT2_SIZE);
	assert(memcmp(data, TEXT2_BYTES, (size_t)TEXT2_SIZE) == 0);

	/* set_data with size > 0 but NULL bytes is rejected, content kept */
	assert(asset_mut_set_data(id, NULL, TEXT2_SIZE) == -1);
	data = asset_get(TEXT_PATH, &sz);
	assert(data != NULL && sz == TEXT2_SIZE);

	/* -------------------------------------------------------------- */
	/* destroy: removes entry; count drops; find returns -1           */
	/* -------------------------------------------------------------- */
	assert(asset_mut_destroy(id) == 0);
	assert(asset_catalog_count() == base_count);
	assert(asset_catalog_find(id, &info) == -1);

	/* -------------------------------------------------------------- */
	/* create rejects duplicate path                                   */
	/* -------------------------------------------------------------- */
	id = asset_mut_create(TEXT_PATH, ASSET_TYPE_TEXT,
			      TEXT_BYTES, TEXT_SIZE);
	assert(id != 0);
	id2 = asset_mut_create(TEXT_PATH, ASSET_TYPE_TEXT,
			       TEXT_BYTES, TEXT_SIZE);
	assert(id2 == 0);  /* duplicate */
	assert(asset_catalog_count() == base_count + 1);
	assert(asset_mut_destroy(id) == 0);

	/* -------------------------------------------------------------- */
	/* set_data / destroy on a non-authored (built-in) id return -1   */
	/* -------------------------------------------------------------- */
	assert(asset_catalog_info(0, &info) == 0);
	assert(info.kind == ASSET_KIND_PRIMITIVE);
	assert(asset_mut_set_data(info.id, TEXT_BYTES, TEXT_SIZE) == -1);
	assert(asset_mut_destroy(info.id) == -1);

	/* -------------------------------------------------------------- */
	/* create with bad args (NULL path) returns 0                     */
	/* -------------------------------------------------------------- */
	assert(asset_mut_create(NULL, ASSET_TYPE_TEXT,
				TEXT_BYTES, TEXT_SIZE) == 0);

	/* -------------------------------------------------------------- */
	/* create fills cache; next create returns 0                      */
	/* -------------------------------------------------------------- */
	{
		char path_buf[32];
		uint32_t ids[64];
		uint32_t j;
		uint32_t created = 0;

		/* Fill all remaining slots. */
		for (j = 0; j < 64; j++) {
			snprintf(path_buf, sizeof(path_buf),
				 "authored://fill_%u.md", j);
			ids[j] = asset_mut_create(path_buf, ASSET_TYPE_TEXT,
						  TEXT_BYTES, TEXT_SIZE);
			if (ids[j] == 0)
				break;
			created++;
		}
		/* At least one slot was filled (could be 0 if already full). */

		/* The next creation must fail (cache full or we ran out). */
		snprintf(path_buf, sizeof(path_buf), "authored://overflow.md");
		assert(asset_mut_create(path_buf, ASSET_TYPE_TEXT,
					TEXT_BYTES, TEXT_SIZE) == 0);

		/* Clean up. */
		for (j = 0; j < created; j++) {
			if (ids[j])
				asset_mut_destroy(ids[j]);
		}
	}

	log_shutdown();
	mem_shutdown();

	printf("asset_mut tests passed\n");
	return 0;
}
