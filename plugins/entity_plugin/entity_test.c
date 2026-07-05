/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "world.h"
#include "scene.h"
#include "memory_api.h"
#include "memory.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* One world instance reused across tests; ~400 KB, too big for the stack. */
static struct world w;

/* Engine allocator for the export tests (world ops themselves allocate
 * nothing; world_export_scene needs an injected memory_api). */
static const struct memory_api test_mem = {
	mem_alloc, mem_alloc_zero, mem_free,
	mem_pool_create, mem_pool_alloc, mem_pool_free, mem_pool_destroy,
};

static int feq(float a, float b)
{
	float d = a - b;

	if (d < 0.0f)
		d = -d;
	return d < 1e-5f;
}

/* Identity-rotation, unit-scale helper for building scene entities. */
static void make_entity(struct scene_entity *e, int32_t parent, uint32_t mask,
			float x, float y, float z, float s)
{
	memset(e, 0, sizeof(*e));
	e->mask        = mask;
	e->parent      = parent;
	e->position.x  = x;
	e->position.y  = y;
	e->position.z  = z;
	e->rotation.w  = 1.0f;       /* identity quaternion */
	e->scale.x     = s;
	e->scale.y     = s;
	e->scale.z     = s;
	e->name_off    = SCENE_NO_NAME;
	e->render_ref  = 0;
}

/*
 * Ingest a three-level chain and check world transforms accumulate down it.
 * names blob: "root\0child\0".
 */
static void test_ingest_and_propagate(void)
{
	static char         names[] = "root\0child";
	struct scene_entity ents[3];
	struct scene        s;

	make_entity(&ents[0], -1, COMPONENT_NAME, 0.0f, 0.0f, 0.0f, 1.0f);
	ents[0].name_off = 0;
	make_entity(&ents[1], 0, COMPONENT_NAME, 1.0f, 0.0f, 0.0f, 1.0f);
	ents[1].name_off = 5;
	make_entity(&ents[2], 1, COMPONENT_RENDER, 2.0f, 3.0f, 4.0f, 2.0f);
	ents[2].render_ref = 42u;

	s.count    = 3;
	s.entities = ents;
	s.names    = names;

	assert(world_ingest_scene(&w, &s) == 0);
	assert(w.count == 3);

	/* Root: world == local. */
	assert(feq(w.world_xform[0].position[0], 0.0f));

	/* Child of root, translated +x. */
	assert(feq(w.world_xform[1].position[0], 1.0f));

	/* Grandchild: parent world pos (1,0,0) + identity*(2,3,4). */
	assert(feq(w.world_xform[2].position[0], 3.0f));
	assert(feq(w.world_xform[2].position[1], 3.0f));
	assert(feq(w.world_xform[2].position[2], 4.0f));
	/* Scale multiplies down the chain: 1 * 1 * 2. */
	assert(feq(w.world_xform[2].scale[0], 2.0f));

	assert(w.render_ref[2] == 42u);
	assert(strcmp(world_entity_name(&w, 0), "root") == 0);
	assert(strcmp(world_entity_name(&w, 1), "child") == 0);
	assert(world_entity_name(&w, 2) == NULL);
}

/* A 90° parent rotation about +Z maps the child's +x offset onto +y. */
static void test_rotation_compose(void)
{
	struct scene_entity ents[2];
	struct scene        s;
	const float         h = 0.70710678f;   /* sin/cos of 45° */

	make_entity(&ents[0], -1, 0u, 0.0f, 0.0f, 0.0f, 1.0f);
	ents[0].rotation.z = h;
	ents[0].rotation.w = h;
	make_entity(&ents[1], 0, 0u, 1.0f, 0.0f, 0.0f, 1.0f);

	s.count    = 2;
	s.entities = ents;
	s.names    = NULL;

	assert(world_ingest_scene(&w, &s) == 0);
	assert(feq(w.world_xform[1].position[0], 0.0f));
	assert(feq(w.world_xform[1].position[1], 1.0f));
	assert(feq(w.world_xform[1].position[2], 0.0f));
}

/* Destroying a parent tombstones its whole subtree but spares siblings. */
static void test_destroy_subtree(void)
{
	struct transform t;
	int32_t          e0, e1, e2, e3;

	memset(&t, 0, sizeof(t));
	t.rotation[3] = 1.0f;
	t.scale[0] = t.scale[1] = t.scale[2] = 1.0f;

	world_reset(&w);
	e0 = world_create_entity(&w, WORLD_NO_PARENT, &t, 0u);  /* root  */
	e1 = world_create_entity(&w, e0, &t, 0u);               /* child */
	e2 = world_create_entity(&w, e0, &t, 0u);               /* sibling */
	e3 = world_create_entity(&w, e1, &t, 0u);               /* grandchild */
	assert(e0 == 0 && e1 == 1 && e2 == 2 && e3 == 3);

	world_destroy_entity(&w, e1);

	/* e1 and its descendant e3 die; e0 and sibling e2 live on. */
	assert(w.alive[e0] == 1);
	assert(w.alive[e1] == 0);
	assert(w.alive[e2] == 1);
	assert(w.alive[e3] == 0);

	/* Indices did not shift: e2 keeps id 2 and a valid parent ref. */
	assert(w.parent[e2] == e0);
	assert(w.alive[w.parent[e2]] == 1);
}

/* create_entity must reject a non-existent or tombstoned parent. */
static void test_create_rejects_bad_parent(void)
{
	struct transform t;
	int32_t          root;

	memset(&t, 0, sizeof(t));
	t.rotation[3] = 1.0f;
	t.scale[0] = t.scale[1] = t.scale[2] = 1.0f;

	world_reset(&w);
	assert(world_create_entity(&w, 5, &t, 0u) == -1);   /* parent >= count */

	root = world_create_entity(&w, WORLD_NO_PARENT, &t, 0u);
	assert(root == 0);
	world_destroy_entity(&w, root);
	assert(world_create_entity(&w, root, &t, 0u) == -1); /* dead parent */
}

/* world_propagate_transforms skips tombstones without touching their column. */
static void test_tick_skips_dead(void)
{
	struct transform t;
	int32_t          e0, e1;

	memset(&t, 0, sizeof(t));
	t.rotation[3] = 1.0f;
	t.scale[0] = t.scale[1] = t.scale[2] = 1.0f;
	t.position[0] = 5.0f;

	world_reset(&w);
	e0 = world_create_entity(&w, WORLD_NO_PARENT, &t, 0u);
	e1 = world_create_entity(&w, e0, &t, 0u);
	world_destroy_entity(&w, e1);

	/* Poison the dead slot; a correct tick must not overwrite it. */
	w.world_xform[e1].position[0] = 999.0f;
	world_tick(&w, 0.0f);
	assert(feq(w.world_xform[e0].position[0], 5.0f));
	assert(feq(w.world_xform[e1].position[0], 999.0f));
}

static void test_empty_scene(void)
{
	struct scene s;

	s.count    = 0;
	s.entities = NULL;
	s.names    = NULL;
	assert(world_ingest_scene(&w, &s) == 0);
	assert(w.count == 0);
}

/* Identity-transform helper for runtime-created entities. */
static void make_identity(struct transform *t)
{
	memset(t, 0, sizeof(*t));
	t->rotation[3] = 1.0f;
	t->scale[0] = t->scale[1] = t->scale[2] = 1.0f;
}

/* set_transform overwrites local and reaches world_xform after a propagate. */
static void test_set_transform(void)
{
	struct transform t;
	int32_t          e;

	make_identity(&t);
	world_reset(&w);
	e = world_create_entity(&w, WORLD_NO_PARENT, &t, 0u);

	t.position[0] = 7.0f;
	world_set_transform(&w, e, &t);
	assert(feq(w.local[e].position[0], 7.0f));
	/* world_xform only catches up on the next propagate. */
	world_propagate_transforms(&w, 0.0f);
	assert(feq(w.world_xform[e].position[0], 7.0f));

	/* Dead / out-of-range ids are no-ops, not writes. */
	world_destroy_entity(&w, e);
	t.position[0] = 99.0f;
	world_set_transform(&w, e, &t);
	world_set_transform(&w, 1000, &t);
	assert(feq(w.local[e].position[0], 7.0f));
}

/* set_name toggles COMPONENT_NAME and round-trips through world_entity_name. */
static void test_set_name(void)
{
	struct transform t;
	int32_t          e;

	make_identity(&t);
	world_reset(&w);
	e = world_create_entity(&w, WORLD_NO_PARENT, &t, 0u);
	assert(world_entity_name(&w, e) == NULL);

	assert(world_set_name(&w, e, "Cube") == 0);
	assert((w.mask[e] & COMPONENT_NAME) != 0);
	assert(strcmp(world_entity_name(&w, e), "Cube") == 0);

	/* Renaming appends fresh bytes; the new name reads back. */
	assert(world_set_name(&w, e, "Sphere") == 0);
	assert(strcmp(world_entity_name(&w, e), "Sphere") == 0);

	/* Empty / NULL clears the component. */
	assert(world_set_name(&w, e, "") == 0);
	assert((w.mask[e] & COMPONENT_NAME) == 0);
	assert(world_entity_name(&w, e) == NULL);

	/* A dead id can't be named. */
	world_destroy_entity(&w, e);
	assert(world_set_name(&w, e, "Ghost") == -1);
}

/* set_render_ref binds a mesh (sets COMPONENT_RENDER); a zero ref unbinds. */
static void test_set_render_ref(void)
{
	struct transform t;
	int32_t          e;

	make_identity(&t);
	world_reset(&w);
	e = world_create_entity(&w, WORLD_NO_PARENT, &t, 0u);

	world_set_render_ref(&w, e, 11u);
	assert(w.render_ref[e] == 11u);
	assert((w.mask[e] & COMPONENT_RENDER) != 0);

	/* Rebinding to a different ref keeps the component and swaps the mesh. */
	world_set_render_ref(&w, e, 22u);
	assert(w.render_ref[e] == 22u);
	assert((w.mask[e] & COMPONENT_RENDER) != 0);

	/* A zero ref unbinds: component cleared, ref reset. */
	world_set_render_ref(&w, e, 0u);
	assert(w.render_ref[e] == 0u);
	assert((w.mask[e] & COMPONENT_RENDER) == 0);
}

/* Selection takes -1 and live ids, rejects stale ones, clears on death. */
static void test_selection(void)
{
	struct transform t;
	int32_t          e0, e1;

	make_identity(&t);
	world_reset(&w);
	assert(world_get_selected(&w) == -1);          /* reset → none */

	e0 = world_create_entity(&w, WORLD_NO_PARENT, &t, 0u);
	e1 = world_create_entity(&w, e0, &t, 0u);

	world_set_selected(&w, e1);
	assert(world_get_selected(&w) == e1);

	/* Out-of-range id is ignored, leaving the selection intact. */
	world_set_selected(&w, 1000);
	assert(world_get_selected(&w) == e1);

	/* Explicit clear. */
	world_set_selected(&w, -1);
	assert(world_get_selected(&w) == -1);

	/* Destroying the selected entity (via cascade) clears the selection. */
	world_set_selected(&w, e1);
	world_destroy_entity(&w, e0);                  /* tombstones e0, e1 */
	assert(world_get_selected(&w) == -1);

	/* Selecting a tombstoned id is rejected. */
	world_set_selected(&w, e1);
	assert(world_get_selected(&w) == -1);
}

/*
 * world_export_scene drops tombstoned slots and remaps parent indices onto the
 * compacted ordering, re-packing names gap-free.
 */
static void test_export_compaction(void)
{
	struct transform t;
	struct scene    *s;
	int32_t          e0, e1, e2;

	make_identity(&t);
	world_reset(&w);
	e0 = world_create_entity(&w, WORLD_NO_PARENT, &t, 0u);  /* root */
	e1 = world_create_entity(&w, e0, &t, 0u);               /* killed subtree */
	e2 = world_create_entity(&w, e0, &t, 0u);               /* survivor */
	(void)world_create_entity(&w, e1, &t, 0u);              /* grandchild dies */
	world_set_name(&w, e0, "root");
	world_set_name(&w, e2, "keep");
	world_set_render_ref(&w, e2, 7u);
	world_destroy_entity(&w, e1);                           /* kills e1 + child */

	s = world_export_scene(&w, &test_mem);
	assert(s != NULL);
	assert(s->count == 2);                     /* e0, e2 survive */

	/* e0 -> compacted 0 (root); e2 -> compacted 1, parent remapped to 0. */
	assert(s->entities[0].parent == -1);
	assert(strcmp(s->names + s->entities[0].name_off, "root") == 0);
	assert(s->entities[1].parent == 0);
	assert(strcmp(s->names + s->entities[1].name_off, "keep") == 0);
	assert((s->entities[1].mask & COMPONENT_RENDER) != 0);
	assert(s->entities[1].render_ref == 7u);

	test_mem.free(s->entities);
	test_mem.free(s->names);
	test_mem.free(s);
}

/* export then re-ingest reproduces the world column-for-column (no tombstones). */
static void test_export_ingest_roundtrip(void)
{
	static struct world w2;
	static char         names[] = "root\0child";
	struct scene_entity ents[3];
	struct scene        s0;
	struct scene       *s;
	uint32_t            i;

	make_entity(&ents[0], -1, COMPONENT_NAME, 0.0f, 0.0f, 0.0f, 1.0f);
	ents[0].name_off = 0;
	make_entity(&ents[1], 0, COMPONENT_NAME, 1.0f, 0.0f, 0.0f, 1.0f);
	ents[1].name_off = 5;
	make_entity(&ents[2], 1, COMPONENT_RENDER, 2.0f, 3.0f, 4.0f, 2.0f);
	ents[2].render_ref = 42u;
	s0.count    = 3;
	s0.entities = ents;
	s0.names    = names;
	assert(world_ingest_scene(&w, &s0) == 0);

	s = world_export_scene(&w, &test_mem);
	assert(s != NULL && s->count == 3);
	assert(world_ingest_scene(&w2, s) == 0);

	assert(w2.count == w.count);
	for (i = 0; i < w.count; i++) {
		assert(w2.mask[i] == w.mask[i]);
		assert(w2.parent[i] == w.parent[i]);
		assert(w2.render_ref[i] == w.render_ref[i]);
		assert(feq(w2.local[i].position[0], w.local[i].position[0]));
	}
	assert(strcmp(world_entity_name(&w2, 0), "root") == 0);
	assert(strcmp(world_entity_name(&w2, 1), "child") == 0);
	assert(world_entity_name(&w2, 2) == NULL);

	test_mem.free(s->entities);
	test_mem.free(s->names);
	test_mem.free(s);
}

int main(void)
{
	mem_init();

	test_ingest_and_propagate();
	test_rotation_compose();
	test_destroy_subtree();
	test_create_rejects_bad_parent();
	test_tick_skips_dead();
	test_empty_scene();
	test_set_transform();
	test_set_name();
	test_set_render_ref();
	test_selection();
	test_export_compaction();
	test_export_ingest_roundtrip();

	mem_shutdown();
	printf("entity tests passed\n");
	return 0;
}
