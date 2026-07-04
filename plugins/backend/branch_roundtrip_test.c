/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "branch_host.h"
#include "branch_api.h"
#include "entity_api.h"
#include "asset_api.h"
#include "memory_api.h"
#include "memory.h"
#include "subsystem_manager.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/*
 * End-to-end round-trip test for the branching runtime (#213 proof-of-life,
 * data-runtime layer).  The gate test (branch_host_test.c) drives the vtable
 * over STUB subsystems that capture nothing; this test stands up fake "scene",
 * "asset", and "asset_mut" subsystems backed by in-test state, registers them
 * on a real subsystem_manager, and drives the actual serialize -> commit ->
 * fork -> switch -> ingest path.
 *
 * It proves the epic's headline live-switch — fork a branch, diverge it, and
 * switching HEAD swaps BOTH the world bytes and the catalog to the target
 * branch's state — natively, without an Emscripten/browser build.  It also
 * pins the manifest path round-trip (#214): an asset the target branch holds
 * but the live catalog lacks is re-injected under its REAL catalog path, not a
 * synthesized "branch-asset:<id>" name.
 */

static const struct memory_api test_mem = {
	mem_alloc, mem_alloc_zero, mem_free,
	mem_pool_create, mem_pool_alloc, mem_pool_free, mem_pool_destroy,
};

/* ------------------------------------------------------------------ */
/* Fake "scene" subsystem: the world is one opaque byte string.        */
/* ------------------------------------------------------------------ */

static uint8_t  g_scene[512];
static uint32_t g_scene_len;

static void scene_set(const char *s)
{
	g_scene_len = (uint32_t)strlen(s);
	memcpy(g_scene, s, g_scene_len);
}

static void *fake_export_scene(uint32_t *out_size)
{
	void *b = mem_alloc(g_scene_len ? g_scene_len : 1);

	if (!b)
		return NULL;
	memcpy(b, g_scene, g_scene_len);
	*out_size = g_scene_len;
	return b;
}

static int32_t fake_ingest_scene(const void *bytes, uint32_t size)
{
	if (size > sizeof(g_scene))
		return -1;
	memcpy(g_scene, bytes, size);
	g_scene_len = size;
	return 0;
}

static const struct entity_api fake_scene_api = {
	.export_scene_bytes = fake_export_scene,
	.ingest_scene_bytes = fake_ingest_scene,
};

/* ------------------------------------------------------------------ */
/* Fake "asset" + "asset_mut" subsystems: a small authored catalog.    */
/* ------------------------------------------------------------------ */

struct fake_asset {
	uint32_t id;
	char     path[64];
	int32_t  type;
	uint8_t  data[128];
	uint32_t size;
};

static struct fake_asset g_cat[16];
static uint32_t          g_cat_n;

static int32_t cat_idx(uint32_t id)
{
	uint32_t i;

	for (i = 0; i < g_cat_n; i++) {
		if (g_cat[i].id == id)
			return (int32_t)i;
	}
	return -1;
}

static void cat_fill_info(const struct fake_asset *a, struct asset_info *out)
{
	memset(out, 0, sizeof(*out));
	out->path   = a->path;
	out->size   = a->size;
	out->type   = a->type;
	out->id     = a->id;
	out->origin = ASSET_ORIGIN_AUTHORED;
}

static uint32_t fake_count(void)
{
	return g_cat_n;
}

static int32_t fake_info(uint32_t i, struct asset_info *out)
{
	if (i >= g_cat_n || !out)
		return -1;
	cat_fill_info(&g_cat[i], out);
	return 0;
}

static int32_t fake_find(uint32_t id, struct asset_info *out)
{
	int32_t i = cat_idx(id);

	if (i < 0 || !out)
		return -1;
	cat_fill_info(&g_cat[i], out);
	return 0;
}

static const void *fake_get_data(uint32_t id, uint32_t *out_size)
{
	int32_t i = cat_idx(id);

	if (i < 0)
		return NULL;
	if (out_size)
		*out_size = g_cat[i].size;
	return g_cat[i].data;
}

static const struct asset_api fake_asset_api = {
	.count    = fake_count,
	.info     = fake_info,
	.find     = fake_find,
	.get_data = fake_get_data,
};

static int32_t fake_inject(uint32_t id, const char *path, int32_t type,
			   const void *bytes, uint32_t size)
{
	struct fake_asset *a;

	if (id == 0 || cat_idx(id) >= 0 || g_cat_n >= 16)
		return -1;
	if (size > sizeof(a->data))
		return -1;

	a = &g_cat[g_cat_n++];
	a->id   = id;
	a->type = type;
	a->size = size;
	memcpy(a->data, bytes, size);
	snprintf(a->path, sizeof(a->path), "%s", path ? path : "");
	return 0;
}

static int32_t fake_set_data(uint32_t id, const void *bytes, uint32_t size)
{
	int32_t i = cat_idx(id);

	if (i < 0 || size > sizeof(g_cat[i].data))
		return -1;
	memcpy(g_cat[i].data, bytes, size);
	g_cat[i].size = size;
	return 0;
}

static int32_t fake_destroy(uint32_t id)
{
	int32_t i = cat_idx(id);

	if (i < 0)
		return -1;
	g_cat[i] = g_cat[--g_cat_n];   /* swap-remove, like the real plugin */
	return 0;
}

static const struct asset_mut_api fake_mut_api = {
	.set_data = fake_set_data,
	.destroy  = fake_destroy,
	.inject   = fake_inject,
};

/* Directly seed an authored asset (simulates the user authoring one). */
static void author_asset(uint32_t id, const char *path, const char *bytes)
{
	int32_t rc = fake_inject(id, path, ASSET_TYPE_TEXT,
				 bytes, (uint32_t)strlen(bytes));
	assert(rc == 0);
}

/* ------------------------------------------------------------------ */

static const struct subsystem terminator = { NULL, NULL, NULL, NULL, NULL, 0 };

static void flush_via_debounce(const struct branch_api *br)
{
	int i;

	br->mark_dirty();
	for (i = 0; i < 200; i++)
		branch_host_tick();
}

static int asset_path_is(uint32_t id, const char *want)
{
	int32_t i = cat_idx(id);

	return i >= 0 && strcmp(g_cat[i].path, want) == 0;
}

static int scene_is(const char *want)
{
	return g_scene_len == strlen(want) &&
	       memcmp(g_scene, want, g_scene_len) == 0;
}

int main(void)
{
	struct subsystem_manager mgr;
	const struct branch_api *br;
	struct subsystem         scene_sub = { "scene", &fake_scene_api,
					       NULL, NULL, NULL, 0 };
	struct subsystem         asset_sub = { "asset", &fake_asset_api,
					       NULL, NULL, NULL, 0 };
	struct subsystem         mut_sub   = { "asset_mut", &fake_mut_api,
					       NULL, NULL, NULL, 0 };

	subsystem_manager_init(&mgr, &terminator);
	subsystem_manager_register(&mgr, &scene_sub);
	subsystem_manager_register(&mgr, &asset_sub);
	subsystem_manager_register(&mgr, &mut_sub);

	assert(branch_host_init(&mgr, &test_mem) == 0);
	br = branch_host_api();
	assert(br != NULL);

	/* Live state for `main`: scene A + one authored asset. */
	scene_set("SCENE-A");
	author_asset(1, "player.md", "P0");

	/* First flush on an empty DB bootstraps `main` from the live state. */
	flush_via_debounce(br);
	assert(br->branch_count() == 1);
	assert(br->branch_active() == 0);
	assert(br->snapshot_count() >= 1);
	printf("PASS: first save bootstraps main from live world + catalog\n");

	/* Fork `exp` off HEAD (does not switch), then switch onto it. */
	assert(br->branch_fork("exp", BRANCH_FROM_HEAD) == 1);
	assert(br->branch_switch(1) == 0);
	assert(br->branch_active() == 1);
	assert(scene_is("SCENE-A"));
	assert(g_cat_n == 1 && asset_path_is(1, "player.md"));
	printf("PASS: fork from head + switch reproduces the source state\n");

	/* Diverge `exp`: new scene + a brand-new authored asset, then save. */
	scene_set("SCENE-B");
	author_asset(2, "enemy.md", "E0");
	flush_via_debounce(br);

	/* Switch back to `main`: the world and catalog swap to main's state —
	 * the asset unique to `exp` is dropped. */
	assert(br->branch_switch(0) == 0);
	assert(scene_is("SCENE-A"));
	assert(g_cat_n == 1);
	assert(asset_path_is(1, "player.md"));
	assert(cat_idx(2) < 0);
	printf("PASS: switch to main swaps state, drops exp's asset\n");

	/*
	 * Switch to `exp`: its unique asset (id 2) is not in the live catalog,
	 * so ingest INJECTS it — and it must come back under its real path
	 * "enemy.md", proving the manifest path round-trip (not the synthetic
	 * "branch-asset:2" fallback).
	 */
	assert(br->branch_switch(1) == 0);
	assert(scene_is("SCENE-B"));
	assert(g_cat_n == 2);
	assert(asset_path_is(1, "player.md"));
	assert(asset_path_is(2, "enemy.md"));
	printf("PASS: switch to exp re-injects the asset at its real path\n");

	branch_host_shutdown();
	subsystem_manager_shutdown(&mgr);
	printf("ALL branch_roundtrip TESTS PASSED\n");
	return 0;
}
