/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "scene_edit.h"
#include "world.h"
#include "edit.h"
#include "memory.h"
#include "memory_api.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * Drives the same record path the scene plugin uses: a real edit_history behind
 * a minimal edit_api vtable, so create/destroy/transform/name each land as an
 * undoable command. Undo/redo are called on the history directly; the memento's
 * apply/revert restore the test world. Run under ASan to catch snapshot leaks.
 */
static struct edit_history g_th;
static struct world        g_w;

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

static void reset(void)
{
	edit_history_clear(&g_th);	/* free any retained mementos */
	edit_history_reset(&g_th);
	world_reset(&g_w);
}

static struct transform xform(float x, float y, float z)
{
	struct transform t;

	memset(&t, 0, sizeof(t));
	t.position[0] = x;
	t.position[1] = y;
	t.position[2] = z;
	t.rotation[3] = 1.0f;
	t.scale[0] = t.scale[1] = t.scale[2] = 1.0f;
	return t;
}

static int feq(float a, float b)
{
	float d = a - b;

	return (d < 0.0f ? -d : d) < 1e-5f;
}

static int32_t live(int32_t id)
{
	return id >= 0 && (uint32_t)id < g_w.count && g_w.alive[id];
}

/* --- edit wrappers mirroring entity_plugin.c exactly --- */

static int32_t do_create(int32_t parent, struct transform t)
{
	struct world_snapshot *before = world_snapshot_capture(&g_w, &g_tmem);
	int32_t                id;

	id = world_create_entity(&g_w, parent, &t, 0);
	if (id >= 0)
		scene_edit_record(&g_tapi, &g_tmem, &g_w, before,
				  "Create Entity", 0);
	else
		world_snapshot_free(before, &g_tmem);
	return id;
}

static void do_destroy(int32_t id)
{
	int32_t                ok     = live(id);
	struct world_snapshot *before =
		ok ? world_snapshot_capture(&g_w, &g_tmem) : NULL;

	world_destroy_entity(&g_w, id);
	if (ok)
		scene_edit_record(&g_tapi, &g_tmem, &g_w, before,
				  "Delete Entity", 0);
}

static void do_move(int32_t id, struct transform t)
{
	int32_t                ok     = live(id);
	struct world_snapshot *before =
		ok ? world_snapshot_capture(&g_w, &g_tmem) : NULL;

	world_set_transform(&g_w, id, &t);
	if (ok)
		scene_edit_record(&g_tapi, &g_tmem, &g_w, before, "Move Entity",
				  scene_edit_key(id, SCENE_EDIT_TRANSFORM));
}

static void do_rename(int32_t id, const char *name)
{
	struct world_snapshot *before = world_snapshot_capture(&g_w, &g_tmem);

	if (world_set_name(&g_w, id, name) == 0)
		scene_edit_record(&g_tapi, &g_tmem, &g_w, before,
				  "Rename Entity",
				  scene_edit_key(id, SCENE_EDIT_NAME));
	else
		world_snapshot_free(before, &g_tmem);
}

/* AC: create produces an undoable entry; undo removes it, redo re-adds. */
static void test_create(void)
{
	int32_t id;

	reset();
	id = do_create(WORLD_NO_PARENT, xform(1.0f, 2.0f, 3.0f));
	assert(id == 0 && g_w.count == 1 && g_w.alive[0]);
	assert(edit_history_can_undo(&g_th));

	assert(edit_history_undo(&g_th) == 1);
	assert(g_w.count == 0);				/* create removed */

	assert(edit_history_redo(&g_th) == 1);
	assert(g_w.count == 1 && g_w.alive[0]);
	assert(feq(g_w.local[0].position[0], 1.0f));
	printf("ok: create undo/redo\n");
}

/* AC: set_transform is undoable. */
static void test_transform(void)
{
	int32_t id;

	reset();
	id = do_create(WORLD_NO_PARENT, xform(0.0f, 0.0f, 0.0f));
	do_move(id, xform(5.0f, 6.0f, 7.0f));
	assert(feq(g_w.local[id].position[0], 5.0f));

	assert(edit_history_undo(&g_th) == 1);		/* undo the move */
	assert(feq(g_w.local[id].position[0], 0.0f));
	assert(edit_history_redo(&g_th) == 1);
	assert(feq(g_w.local[id].position[1], 6.0f));
	printf("ok: transform undo/redo\n");
}

/* AC: set_name is undoable (restores the prior name + component bit). */
static void test_name(void)
{
	int32_t id;

	reset();
	id = do_create(WORLD_NO_PARENT, xform(0.0f, 0.0f, 0.0f));
	do_rename(id, "Hero");
	assert(world_entity_name(&g_w, (uint32_t)id) &&
	       strcmp(world_entity_name(&g_w, (uint32_t)id), "Hero") == 0);

	assert(edit_history_undo(&g_th) == 1);
	assert(world_entity_name(&g_w, (uint32_t)id) == NULL);
	assert(!(g_w.mask[id] & COMPONENT_NAME));

	assert(edit_history_redo(&g_th) == 1);
	assert(strcmp(world_entity_name(&g_w, (uint32_t)id), "Hero") == 0);
	printf("ok: name undo/redo\n");
}

/* AC: undoing a destroy restores the entity AND its whole subtree. */
static void test_destroy_cascade(void)
{
	int32_t root, child, grand;

	reset();
	root  = do_create(WORLD_NO_PARENT, xform(0.0f, 0.0f, 0.0f));
	child = do_create(root, xform(0.0f, 0.0f, 0.0f));
	grand = do_create(child, xform(0.0f, 0.0f, 0.0f));
	assert(g_w.count == 3);

	do_destroy(root);
	assert(!g_w.alive[root] && !g_w.alive[child] && !g_w.alive[grand]);

	assert(edit_history_undo(&g_th) == 1);
	assert(g_w.alive[root] && g_w.alive[child] && g_w.alive[grand]);
	assert(g_w.parent[child] == root && g_w.parent[grand] == child);

	assert(edit_history_redo(&g_th) == 1);
	assert(!g_w.alive[root] && !g_w.alive[child] && !g_w.alive[grand]);
	printf("ok: destroy cascade undo/redo\n");
}

/* AC: undo/redo restores the selection active at that point. */
static void test_selection(void)
{
	int32_t a, b;

	reset();
	a = do_create(WORLD_NO_PARENT, xform(0.0f, 0.0f, 0.0f));
	b = do_create(WORLD_NO_PARENT, xform(0.0f, 0.0f, 0.0f));
	world_set_selected(&g_w, b);
	assert(world_get_selected(&g_w) == b);

	/* Destroying the selection clears it... */
	do_destroy(b);
	assert(!g_w.alive[b] && world_get_selected(&g_w) == -1);

	/* ...and undo brings back both the entity and the selection. */
	assert(edit_history_undo(&g_th) == 1);
	assert(g_w.alive[b] && world_get_selected(&g_w) == b);
	(void)a;
	printf("ok: selection restore\n");
}

/* AC: a continuous drag (many same-entity transform edits) is one entry. */
static void test_coalesce_one_entry(void)
{
	int32_t id;

	reset();
	id = do_create(WORLD_NO_PARENT, xform(0.0f, 0.0f, 0.0f));
	do_move(id, xform(1.0f, 0.0f, 0.0f));
	do_move(id, xform(2.0f, 0.0f, 0.0f));
	do_move(id, xform(3.0f, 0.0f, 0.0f));
	assert(feq(g_w.local[id].position[0], 3.0f));

	/* One undo reverts the whole drag to the pre-drag transform... */
	assert(edit_history_undo(&g_th) == 1);
	assert(feq(g_w.local[id].position[0], 0.0f));
	/* ...leaving only the create entry — proof the 3 moves collapsed. */
	assert(edit_history_undo(&g_th) == 1);
	assert(g_w.count == 0);
	assert(edit_history_undo(&g_th) == 0);
	printf("ok: drag coalesces to one entry\n");
}

/* AC: with "edit" unavailable, the scene api still mutates (no hard dep). */
static void test_no_edit_service(void)
{
	struct world_snapshot *before;

	reset();
	/* NULL edit: record is a no-op that still frees the snapshot. */
	before = world_snapshot_capture(&g_w, &g_tmem);
	world_create_entity(&g_w, WORLD_NO_PARENT, &(struct transform){
		.rotation = {0, 0, 0, 1}, .scale = {1, 1, 1} }, 0);
	scene_edit_record(NULL, &g_tmem, &g_w, before, "Create Entity", 0);

	assert(g_w.count == 1 && g_w.alive[0]);		/* mutation happened */
	assert(!edit_history_can_undo(&g_th));		/* nothing recorded */
	printf("ok: no-edit-service mutates without recording\n");
}

int main(void)
{
	mem_init();
	test_create();
	test_transform();
	test_name();
	test_destroy_cascade();
	test_selection();
	test_coalesce_one_entry();
	test_no_edit_service();
	edit_history_clear(&g_th);	/* free retained mementos for ASan */
	printf("all scene_edit tests passed\n");
	return 0;
}
