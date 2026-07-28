/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "scene_save.h"
#include "world.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * The writer, against a hand-built world and a fake catalog.
 *
 * Deliberately no s7 here: this asserts what the text says, and the assertion
 * that it loads back is scene_script_test's, which owns the reader. Splitting
 * them that way means a round-trip failure names which half broke instead of
 * reporting that the pair disagrees.
 */

static struct world g_w;
static char         g_out[16 * 1024];

/* A catalog of three entries with ids 1, 2 and 3. */
static struct asset_info g_entries[] = {
	{ .path = "builtin://mesh/box",         .id = 1 },
	{ .path = "builtin://material/checker", .id = 2 },
	{ .path = "builtin://script/spin",      .id = 3 },
};

static int32_t fake_find(uint32_t id, struct asset_info *out)
{
	size_t i;

	for (i = 0; i < sizeof(g_entries) / sizeof(g_entries[0]); i++) {
		if (g_entries[i].id == id) {
			*out = g_entries[i];
			return 0;
		}
	}
	return -1;
}

static const struct asset_api g_assets = { .find = fake_find };

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

static int32_t spawn(int32_t parent, float x, float y, float z)
{
	struct transform t = xform(x, y, z);

	return world_create_entity(&g_w, parent, &t, 0u);
}

static void test_empty_world(void)
{
	int32_t n;

	world_reset(&g_w);
	n = scene_save_write(&g_w, &g_assets, "empty", g_out, sizeof(g_out));

	assert(n > 0);
	assert(strcmp(g_out, "(scene empty)\n") == 0);
	printf("ok: an empty world is still a well-formed scene form\n");
}

static void test_entity_clauses(void)
{
	int32_t id;

	world_reset(&g_w);
	id = spawn(WORLD_NO_PARENT, 1.5f, -2.0f, 0.25f);
	world_set_name(&g_w, id, "board");
	world_set_render_ref(&g_w, id, 1u);
	world_set_material_ref(&g_w, id, 2u);
	world_set_script_ref(&g_w, id, 3u);

	assert(scene_save_write(&g_w, &g_assets, "one", g_out,
				sizeof(g_out)) > 0);

	assert(strstr(g_out, "(name \"board\")"));
	assert(strstr(g_out, "(mesh \"builtin://mesh/box\")"));
	assert(strstr(g_out, "(material \"builtin://material/checker\")"));
	assert(strstr(g_out, "(script \"builtin://script/spin\")"));
	assert(strstr(g_out, "(at 1.5 -2 0.25)"));
	assert(strstr(g_out, "(quat 0 0 0 1)"));
	assert(strstr(g_out, "(scale 1 1 1)"));
	printf("ok: every set clause is written, and only the set ones\n");
}

static void test_hierarchy_nests(void)
{
	int32_t parent, child, grandchild;
	const char *at_parent, *at_child, *kids;

	world_reset(&g_w);
	parent = spawn(WORLD_NO_PARENT, 0.0f, 0.0f, 0.0f);
	child = spawn(parent, 1.0f, 0.0f, 0.0f);
	grandchild = spawn(child, 2.0f, 0.0f, 0.0f);
	assert(parent >= 0 && child >= 0 && grandchild >= 0);

	assert(scene_save_write(&g_w, &g_assets, "tree", g_out,
				sizeof(g_out)) > 0);

	/*
	 * The child is inside the parent's (children ...), not beside it.
	 * checked by position rather than by counting parens: a flattened tree
	 * would still contain every clause, and would load back as three roots.
	 */
	kids = strstr(g_out, "(children");
	at_parent = strstr(g_out, "(at 0 0 0)");
	at_child = strstr(g_out, "(at 1 0 0)");
	assert(kids && at_parent && at_child);
	assert(at_parent < kids && kids < at_child);
	assert(strstr(g_out, "(at 2 0 0)") > at_child);
	printf("ok: hierarchy nests through (children ...)\n");
}

static void test_tombstones_are_not_written(void)
{
	int32_t keep, drop;

	world_reset(&g_w);
	keep = spawn(WORLD_NO_PARENT, 0.0f, 0.0f, 0.0f);
	drop = spawn(WORLD_NO_PARENT, 9.0f, 9.0f, 9.0f);
	world_set_name(&g_w, keep, "keep");
	world_set_name(&g_w, drop, "drop");
	world_destroy_entity(&g_w, drop);

	assert(scene_save_write(&g_w, &g_assets, "graves", g_out,
				sizeof(g_out)) > 0);

	assert(strstr(g_out, "\"keep\""));
	assert(!strstr(g_out, "\"drop\""));
	printf("ok: a tombstoned entity is not saved\n");
}

static void test_destroyed_subtree_is_not_written(void)
{
	int32_t parent, child;

	world_reset(&g_w);
	parent = spawn(WORLD_NO_PARENT, 0.0f, 0.0f, 0.0f);
	child = spawn(parent, 0.0f, 0.0f, 0.0f);
	world_set_name(&g_w, child, "orphan");
	world_destroy_entity(&g_w, parent);

	assert(scene_save_write(&g_w, &g_assets, "gone", g_out,
				sizeof(g_out)) > 0);

	/* The cascade tombstones the child too, so neither may appear. */
	assert(!strstr(g_out, "\"orphan\""));
	assert(strcmp(g_out, "(scene gone)\n") == 0);
	printf("ok: a destroyed subtree leaves nothing behind\n");
}

static void test_unresolvable_ref_is_skipped(void)
{
	int32_t id;

	world_reset(&g_w);
	id = spawn(WORLD_NO_PARENT, 0.0f, 0.0f, 0.0f);
	world_set_name(&g_w, id, "ghost");
	world_set_render_ref(&g_w, id, 999u);	/* not in the catalog */

	assert(scene_save_write(&g_w, &g_assets, "ghosts", g_out,
				sizeof(g_out)) > 0);

	/* The clause goes; the entity stays. Never a bare numeric ref. */
	assert(!strstr(g_out, "(mesh"));
	assert(!strstr(g_out, "999"));
	assert(strstr(g_out, "\"ghost\""));
	printf("ok: an unresolvable asset ref drops its clause, not the entity\n");
}

static void test_no_catalog(void)
{
	int32_t id;

	world_reset(&g_w);
	id = spawn(WORLD_NO_PARENT, 0.0f, 0.0f, 0.0f);
	world_set_render_ref(&g_w, id, 1u);

	assert(scene_save_write(&g_w, NULL, "bare", g_out, sizeof(g_out)) > 0);
	assert(!strstr(g_out, "(mesh"));
	printf("ok: no catalog means no binding clauses, not a failed save\n");
}

static void test_float_precision_round_trips(void)
{
	int32_t     id;
	const char *at;
	float       awkward = 0.1f;	/* not representable in decimal */

	world_reset(&g_w);
	id = spawn(WORLD_NO_PARENT, awkward, 1.0f / 3.0f, 1e-8f);
	assert(id >= 0);

	assert(scene_save_write(&g_w, &g_assets, "precise", g_out,
				sizeof(g_out)) > 0);

	/*
	 * %.9g, so the text reads back as the same binary32. "0.1" would not —
	 * it is the shortest decimal that *displays* as 0.1, and re-reading it
	 * lands on a different float than the one stored.
	 */
	at = strstr(g_out, "(at ");
	assert(at);
	assert(strstr(at, "0.100000001"));
	assert(strstr(at, "0.333333343"));
	printf("ok: floats are written at round-trip precision\n");
}

static void test_string_escaping(void)
{
	int32_t id;

	world_reset(&g_w);
	id = spawn(WORLD_NO_PARENT, 0.0f, 0.0f, 0.0f);
	world_set_name(&g_w, id, "he said \"hi\" \\ then\nleft");

	assert(scene_save_write(&g_w, &g_assets, "quoted", g_out,
				sizeof(g_out)) > 0);

	assert(strstr(g_out, "(name \"he said \\\"hi\\\" \\\\ thenleft\")"));
	printf("ok: quotes and backslashes escape, control characters drop\n");
}

static void test_bad_scene_name_falls_back(void)
{
	world_reset(&g_w);

	assert(scene_save_write(&g_w, &g_assets, "has space", g_out,
				sizeof(g_out)) > 0);
	assert(strcmp(g_out, "(scene scene)\n") == 0);

	assert(scene_save_write(&g_w, &g_assets, "9lives", g_out,
				sizeof(g_out)) > 0);
	assert(strcmp(g_out, "(scene scene)\n") == 0);

	assert(scene_save_write(&g_w, &g_assets, NULL, g_out,
				sizeof(g_out)) > 0);
	assert(strcmp(g_out, "(scene scene)\n") == 0);

	assert(scene_save_write(&g_w, &g_assets, "my-scene_2", g_out,
				sizeof(g_out)) > 0);
	assert(strcmp(g_out, "(scene my-scene_2)\n") == 0);
	printf("ok: a name that is not a symbol falls back whole\n");
}

static void test_overflow_fails_rather_than_truncates(void)
{
	char small[64];

	world_reset(&g_w);
	spawn(WORLD_NO_PARENT, 0.0f, 0.0f, 0.0f);
	spawn(WORLD_NO_PARENT, 1.0f, 1.0f, 1.0f);

	assert(scene_save_write(&g_w, &g_assets, "big", small,
				sizeof(small)) == -1);
	printf("ok: a scene that does not fit is an error, not a short form\n");
}

static void test_bad_arguments(void)
{
	world_reset(&g_w);

	assert(scene_save_write(NULL, &g_assets, "x", g_out, sizeof(g_out)) == -1);
	assert(scene_save_write(&g_w, &g_assets, "x", NULL, 16) == -1);
	assert(scene_save_write(&g_w, &g_assets, "x", g_out, 0) == -1);
	printf("ok: unusable arguments are refused\n");
}

static void test_depth_limit(void)
{
	int32_t parent = WORLD_NO_PARENT;
	int32_t i;

	world_reset(&g_w);
	for (i = 0; i <= SCENE_SAVE_MAX_DEPTH + 2; i++)
		parent = spawn(parent, 0.0f, 0.0f, 0.0f);

	assert(scene_save_write(&g_w, &g_assets, "deep", g_out,
				sizeof(g_out)) == -1);
	printf("ok: a hierarchy past the depth limit fails the save\n");
}

int main(void)
{
	test_empty_world();
	test_entity_clauses();
	test_hierarchy_nests();
	test_tombstones_are_not_written();
	test_destroyed_subtree_is_not_written();
	test_unresolvable_ref_is_skipped();
	test_no_catalog();
	test_float_precision_round_trips();
	test_string_escaping();
	test_bad_scene_name_falls_back();
	test_overflow_fails_rather_than_truncates();
	test_bad_arguments();
	test_depth_limit();
	printf("all scene_save tests passed\n");
	return 0;
}
