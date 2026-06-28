/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "world.h"
#include "scene.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* One world instance reused across tests; ~400 KB, too big for the stack. */
static struct world w;

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

int main(void)
{
	test_ingest_and_propagate();
	test_rotation_compose();
	test_destroy_subtree();
	test_create_rejects_bad_parent();
	test_tick_skips_dead();
	test_empty_scene();

	printf("entity tests passed\n");
	return 0;
}
