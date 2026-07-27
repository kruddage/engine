/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "asset_edit.h"
#include "asset.h"
#include "edit.h"
#include "log.h"
#include "memory.h"
#include "memory_api.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * Drives the same record path the asset plugin uses: a real edit_history behind
 * a minimal edit_api vtable, so create / set_data / destroy each land as an
 * undoable command against the live asset catalog. Undo/redo are called on the
 * history directly; the memento's apply/revert re-drive the catalog. Run under
 * ASan to catch snapshot leaks.
 */
static struct edit_history g_th;

/* Snapshots allocate through the real memory module, same as the plugin. */
static const struct memory_api g_tmem = {
	.alloc      = mem_alloc,
	.alloc_zero = mem_alloc_zero,
	.free       = mem_free,
};

static void th_push(const struct edit_cmd *cmd)
{
	edit_history_push(&g_th, cmd);
}

/* Only push() is exercised through the vtable; the rest stay NULL. */
static const struct edit_api g_tapi = { .push = th_push };

#define P1 "authored://note.md"
#define B1 "# One\n"
#define B1_SZ ((uint32_t)(sizeof(B1) - 1))
#define B2 "# One\nmore\n"
#define B2_SZ ((uint32_t)(sizeof(B2) - 1))
#define B3 "# One\nmore\nstill\n"
#define B3_SZ ((uint32_t)(sizeof(B3) - 1))

/* --- edit wrappers mirroring asset_plugin.c rec_* exactly --- */

static uint32_t do_create(const char *path, const void *bytes, uint32_t size)
{
	struct asset_snapshot *before = asset_snapshot_absent(&g_tmem);
	uint32_t               id;

	id = asset_mut_create(path, ASSET_TYPE_TEXT, bytes, size);
	if (id != 0)
		asset_edit_record(&g_tapi, &g_tmem, id, before,
				  "Create Asset", 0);
	else
		asset_snapshot_free(before, &g_tmem);
	return id;
}

static int32_t do_set_data(uint32_t id, const void *bytes, uint32_t size)
{
	struct asset_snapshot *before = asset_snapshot_capture(id, &g_tmem);
	int32_t                rc;

	rc = asset_mut_set_data(id, bytes, size);
	if (rc == 0)
		asset_edit_record(&g_tapi, &g_tmem, id, before, "Edit Asset",
				  asset_edit_key(id));
	else
		asset_snapshot_free(before, &g_tmem);
	return rc;
}

static int32_t do_destroy(uint32_t id)
{
	struct asset_snapshot *before = asset_snapshot_capture(id, &g_tmem);
	int32_t                rc;

	rc = asset_mut_destroy(id);
	if (rc == 0)
		asset_edit_record(&g_tapi, &g_tmem, id, before,
				  "Delete Asset", 0);
	else
		asset_snapshot_free(before, &g_tmem);
	return rc;
}

static void reset(void)
{
	edit_history_clear(&g_th);	/* free any retained mementos */
	edit_history_reset(&g_th);
}

static int bytes_are(uint32_t id, const void *want, uint32_t want_sz)
{
	uint32_t    sz  = 0;
	const void *got = asset_catalog_get_data(id, &sz);

	if (want_sz == 0)
		return sz == 0;
	return got && sz == want_sz && memcmp(got, want, (size_t)want_sz) == 0;
}

static int is_live(uint32_t id)
{
	struct asset_info info;

	return asset_catalog_find(id, &info) == 0;
}

/* AC: create produces an undoable entry; undo removes it, redo re-adds it. */
static void test_create(void)
{
	uint32_t id;

	reset();
	id = do_create(P1, B1, B1_SZ);
	assert(id != 0 && is_live(id) && bytes_are(id, B1, B1_SZ));
	assert(edit_history_can_undo(&g_th));

	assert(edit_history_undo(&g_th) == 1);
	assert(!is_live(id));				/* create removed */

	assert(edit_history_redo(&g_th) == 1);
	/* Comes back under the SAME stable id (inject preserves identity). */
	assert(is_live(id) && bytes_are(id, B1, B1_SZ));

	assert(do_destroy(id) == 0);
	printf("ok: create undo/redo\n");
}

/* AC: undoing a text edit restores the prior bytes; redo reapplies. */
static void test_set_data(void)
{
	uint32_t id;

	reset();
	id = do_create(P1, B1, B1_SZ);
	assert(do_set_data(id, B2, B2_SZ) == 0);
	assert(bytes_are(id, B2, B2_SZ));

	assert(edit_history_undo(&g_th) == 1);		/* undo the edit */
	assert(bytes_are(id, B1, B1_SZ));
	assert(edit_history_redo(&g_th) == 1);
	assert(bytes_are(id, B2, B2_SZ));

	assert(do_destroy(id) == 0);
	printf("ok: set_data undo/redo\n");
}

/* AC: undoing a destroy brings the asset back (bytes + id); redo removes it. */
static void test_destroy(void)
{
	uint32_t id;

	reset();
	id = do_create(P1, B2, B2_SZ);
	assert(do_destroy(id) == 0);
	assert(!is_live(id));

	assert(edit_history_undo(&g_th) == 1);
	assert(is_live(id) && bytes_are(id, B2, B2_SZ));

	assert(edit_history_redo(&g_th) == 1);
	assert(!is_live(id));

	/* Bring it back once more so the cleanup path has something to free. */
	assert(edit_history_undo(&g_th) == 1);
	assert(do_destroy(id) == 0);
	printf("ok: destroy undo/redo\n");
}

/*
 * AC: an editing session collapses into sensible coalesced entries — a run of
 * same-asset edits folds into one, so a single undo reverts the whole run.
 */
static void test_coalesce_one_entry(void)
{
	uint32_t id;

	reset();
	id = do_create(P1, B1, B1_SZ);
	assert(do_set_data(id, B2, B2_SZ) == 0);
	assert(do_set_data(id, B3, B3_SZ) == 0);
	assert(bytes_are(id, B3, B3_SZ));

	/* One undo reverts the whole run back to the pre-edit bytes... */
	assert(edit_history_undo(&g_th) == 1);
	assert(bytes_are(id, B1, B1_SZ));
	/* ...leaving only the create entry — proof the edits collapsed. */
	assert(edit_history_undo(&g_th) == 1);
	assert(!is_live(id));
	assert(edit_history_undo(&g_th) == 0);
	printf("ok: edit run coalesces to one entry\n");
}

/*
 * AC: a global timeline records across domains in order — here two distinct
 * assets, walked back newest-first. Different coalesce keys never fold together.
 */
static void test_cross_asset_order(void)
{
	uint32_t a, b;

	reset();
	a = do_create("authored://a.md", B1, B1_SZ);
	b = do_create("authored://b.md", B2, B2_SZ);
	assert(a != 0 && b != 0 && a != b);

	/* Undo removes b (the most recent), then a. */
	assert(edit_history_undo(&g_th) == 1);
	assert(!is_live(b) && is_live(a));
	assert(edit_history_undo(&g_th) == 1);
	assert(!is_live(a));

	/* Redo replays them in creation order. */
	assert(edit_history_redo(&g_th) == 1 && is_live(a));
	assert(edit_history_redo(&g_th) == 1 && is_live(b));

	assert(do_destroy(a) == 0 && do_destroy(b) == 0);
	printf("ok: cross-asset order\n");
}

/* AC: with "edit" unavailable, asset_mut still mutates (no hard dependency). */
static void test_no_edit_service(void)
{
	struct asset_snapshot *before;
	uint32_t               id;

	reset();
	/* NULL edit: record is a no-op that still frees the snapshot. */
	before = asset_snapshot_absent(&g_tmem);
	id = asset_mut_create(P1, ASSET_TYPE_TEXT, B1, B1_SZ);
	asset_edit_record(NULL, &g_tmem, id, before, "Create Asset", 0);

	assert(is_live(id) && bytes_are(id, B1, B1_SZ));	/* mutation happened */
	assert(!edit_history_can_undo(&g_th));		/* nothing recorded */

	assert(asset_mut_destroy(id) == 0);
	printf("ok: no-edit-service mutates without recording\n");
}

int main(void)
{
	mem_init();
	log_init();
	asset_init();

	test_create();
	test_set_data();
	test_destroy();
	test_coalesce_one_entry();
	test_cross_asset_order();
	test_no_edit_service();

	edit_history_clear(&g_th);	/* free retained mementos for ASan */
	log_shutdown();
	mem_shutdown();
	printf("all asset_edit tests passed\n");
	return 0;
}
