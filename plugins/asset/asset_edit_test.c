/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "asset.h"
#include "asset_api.h"
#include "edit.h"
#include "edit_api.h"
#include "log.h"
#include "memory.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/*
 * Drives the same record path asset_mut uses: a real edit_history behind a
 * minimal edit_api vtable bound via asset_edit_bind(), so create/set_data/
 * destroy each land as an undoable command. Undo/redo are called on the
 * history directly. Run under ASan to catch snapshot leaks.
 */
static struct edit_history g_th;

static void th_push(const struct edit_cmd *cmd)
{
	edit_history_push(&g_th, cmd);
}

/* Only push() is exercised through the vtable; the rest stay NULL. */
static const struct edit_api g_tapi = { .push = th_push };

#define TEXT_PATH  "authored://hello.md"
#define TEXT_BYTES "# Hello\nWorld\n"
#define TEXT_SIZE  ((uint32_t)(sizeof(TEXT_BYTES) - 1))

#define TEXT2_BYTES "# Updated\n"
#define TEXT2_SIZE  ((uint32_t)(sizeof(TEXT2_BYTES) - 1))

static void reset(void)
{
	edit_history_clear(&g_th);	/* free any retained mementos */
	edit_history_reset(&g_th);
}

/* AC: create produces an undoable entry; undo removes it, redo re-adds it
 * at the same stable id. */
static void test_create(void)
{
	uint32_t          id;
	struct asset_info info;

	reset();
	asset_edit_bind(&g_tapi);
	id = asset_mut_create(TEXT_PATH, ASSET_TYPE_TEXT, TEXT_BYTES, TEXT_SIZE);
	assert(id != 0);
	assert(edit_history_can_undo(&g_th));

	assert(edit_history_undo(&g_th) == 1);
	assert(asset_catalog_find(id, &info) == -1);	/* create undone */

	assert(edit_history_redo(&g_th) == 1);
	assert(asset_catalog_find(id, &info) == 0);
	assert(info.id == id && info.size == TEXT_SIZE);
	assert(info.origin == ASSET_ORIGIN_AUTHORED);

	assert(asset_mut_destroy(id) == 0);
	printf("ok: create undo/redo\n");
}

/* AC: set_data undo restores the prior bytes; redo restores the edit. */
static void test_set_data(void)
{
	uint32_t    id;
	const void *data;
	uint32_t    sz;

	reset();
	asset_edit_bind(&g_tapi);
	id = asset_mut_create(TEXT_PATH, ASSET_TYPE_TEXT, TEXT_BYTES, TEXT_SIZE);
	assert(id != 0);
	/* Drop the create entry so only the set_data edit is under test. */
	reset();

	assert(asset_mut_set_data(id, TEXT2_BYTES, TEXT2_SIZE) == 0);
	assert(edit_history_can_undo(&g_th));

	assert(edit_history_undo(&g_th) == 1);
	data = asset_catalog_get_data(id, &sz);
	assert(data && sz == TEXT_SIZE &&
	       memcmp(data, TEXT_BYTES, (size_t)TEXT_SIZE) == 0);

	assert(edit_history_redo(&g_th) == 1);
	data = asset_catalog_get_data(id, &sz);
	assert(data && sz == TEXT2_SIZE &&
	       memcmp(data, TEXT2_BYTES, (size_t)TEXT2_SIZE) == 0);

	assert(asset_mut_destroy(id) == 0);
	printf("ok: set_data undo/redo\n");
}

/* AC: a run of saves against the same asset coalesces to one entry. */
static void test_coalesce_one_entry(void)
{
	uint32_t    id;
	const void *data;
	uint32_t    sz;

	reset();
	asset_edit_bind(&g_tapi);
	id = asset_mut_create(TEXT_PATH, ASSET_TYPE_TEXT, TEXT_BYTES, TEXT_SIZE);
	assert(id != 0);
	reset();

	assert(asset_mut_set_data(id, "a", 1) == 0);
	assert(asset_mut_set_data(id, "ab", 2) == 0);
	assert(asset_mut_set_data(id, "abc", 3) == 0);

	/* One undo reverts the whole run to the pre-edit bytes... */
	assert(edit_history_undo(&g_th) == 1);
	data = asset_catalog_get_data(id, &sz);
	assert(data && sz == TEXT_SIZE &&
	       memcmp(data, TEXT_BYTES, (size_t)TEXT_SIZE) == 0);
	/* ...leaving nothing else to undo -- proof the 3 saves collapsed. */
	assert(edit_history_undo(&g_th) == 0);

	assert(asset_mut_destroy(id) == 0);
	printf("ok: saves coalesce to one entry\n");
}

/* AC: destroy inverts to a recreate at the same stable id, bytes intact. */
static void test_destroy(void)
{
	uint32_t          id;
	struct asset_info info;
	const void       *data;
	uint32_t          sz;

	reset();
	asset_edit_bind(&g_tapi);
	id = asset_mut_create(TEXT_PATH, ASSET_TYPE_TEXT, TEXT_BYTES, TEXT_SIZE);
	assert(id != 0);
	reset();

	assert(asset_mut_destroy(id) == 0);
	assert(asset_catalog_find(id, &info) == -1);
	assert(edit_history_can_undo(&g_th));

	assert(edit_history_undo(&g_th) == 1);
	assert(asset_catalog_find(id, &info) == 0);
	assert(info.id == id && info.type == ASSET_TYPE_TEXT);
	assert(info.origin == ASSET_ORIGIN_AUTHORED);
	data = asset_catalog_get_data(id, &sz);
	assert(data && sz == TEXT_SIZE &&
	       memcmp(data, TEXT_BYTES, (size_t)TEXT_SIZE) == 0);

	assert(edit_history_redo(&g_th) == 1);
	assert(asset_catalog_find(id, &info) == -1);	/* destroyed again */
	printf("ok: destroy undo/redo (same id revived)\n");
}

/* AC: with "edit" unavailable, asset_mut still mutates (no hard dep). */
static void test_no_edit_service(void)
{
	uint32_t id;

	reset();
	asset_edit_bind(NULL);
	id = asset_mut_create(TEXT_PATH, ASSET_TYPE_TEXT, TEXT_BYTES, TEXT_SIZE);
	assert(id != 0);			/* mutation happened */
	assert(!edit_history_can_undo(&g_th));	/* nothing recorded */

	assert(asset_mut_set_data(id, TEXT2_BYTES, TEXT2_SIZE) == 0);
	assert(!edit_history_can_undo(&g_th));

	assert(asset_mut_destroy(id) == 0);
	assert(!edit_history_can_undo(&g_th));
	printf("ok: no-edit-service mutates without recording\n");
}

int main(void)
{
	mem_init();
	log_init();
	asset_init();

	test_create();
	test_set_data();
	test_coalesce_one_entry();
	test_destroy();
	test_no_edit_service();

	edit_history_clear(&g_th);	/* free retained mementos for ASan */
	log_shutdown();
	mem_shutdown();

	printf("all asset_edit tests passed\n");
	return 0;
}
