/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "branch.h"
#include "cas.h"
#include "cas_mem.h"
#include "memory_api.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *t_alloc_zero(size_t n)
{
	return calloc(1, n);
}

static const struct memory_api test_mem = {
	malloc, t_alloc_zero, free, NULL, NULL, NULL, NULL,
};

static cas_hash_t put_blob(struct cas *s, const char *bytes)
{
	cas_hash_t h = 0;

	assert(cas_put_blob(s, bytes, (uint32_t)strlen(bytes), &h) == 0);
	return h;
}

static cas_hash_t put_manifest(struct cas *s, const struct cas_entry *e,
			       uint32_t n)
{
	cas_hash_t h = 0;

	assert(cas_put_manifest(s, e, n, &h) == 0);
	return h;
}

/* First save on an empty set bootstraps `main`, active, with no fork base. */
static void test_bootstrap(void)
{
	struct cas       s;
	struct branches  b;
	struct cas_entry a[2];
	cas_hash_t       mA;
	const struct branch_info *main_b;

	assert(cas_mem_init(&s, &test_mem) == 0);
	branches_init(&b, &s);
	assert(branches_count(&b) == 0);
	assert(branches_active(&b) == BRANCH_NONE);

	a[0].id = 1; a[0].kind = 1; a[0].hash = put_blob(&s, "sceneA");
	a[1].id = 2; a[1].kind = 7; a[1].hash = put_blob(&s, "assetA");
	mA = put_manifest(&s, a, 2);

	assert(branches_commit(&b, mA) == 0);
	assert(branches_count(&b) == 1);
	assert(branches_active(&b) == 0);

	main_b = branches_get(&b, 0);
	assert(main_b != NULL);
	assert(strcmp(main_b->name, "main") == 0);
	assert(main_b->manifest == mA);
	assert(main_b->base == CAS_HASH_NONE);

	cas_mem_shutdown(&s);
	printf("PASS: bootstrap mints active main\n");
}

/* Subsequent saves advance the active branch copy-on-write (one branch still). */
static void test_live_save_advance(void)
{
	struct cas       s;
	struct branches  b;
	struct cas_entry a[2], b2[2];
	cas_hash_t       mA, mB;
	uint32_t         before, after;

	assert(cas_mem_init(&s, &test_mem) == 0);
	branches_init(&b, &s);

	a[0].id = 1; a[0].kind = 1; a[0].hash = put_blob(&s, "sceneA");
	a[1].id = 2; a[1].kind = 7; a[1].hash = put_blob(&s, "assetA");
	mA = put_manifest(&s, a, 2);
	assert(branches_commit(&b, mA) == 0);

	before = s.backing.count(s.backing.ctx);   /* 2 blobs + 1 manifest */

	/* Edit asset id 2 only; id 1 keeps its content. */
	b2[0] = a[0];
	b2[1].id = 2; b2[1].kind = 7; b2[1].hash = put_blob(&s, "assetA2");
	mB = put_manifest(&s, b2, 2);

	after = s.backing.count(s.backing.ctx);
	assert(after - before == 2u);              /* CoW: +1 blob, +1 manifest */

	assert(branches_commit(&b, mB) == 0);      /* advances main, not a new branch */
	assert(branches_count(&b) == 1);
	assert(branches_get(&b, 0)->manifest == mB);

	cas_mem_shutdown(&s);
	printf("PASS: live-save advances active branch (CoW)\n");
}

/*
 * Fork a branch off a snapshot, diverge each branch, and switch between them —
 * the working set follows HEAD to the right state.  This is the epic's
 * proof-of-life at the data layer.
 */
static void test_fork_and_switch(void)
{
	struct cas       s;
	struct branches  b;
	struct cas_entry a[2], mainB[2], expB[2], ws[8];
	cas_hash_t       mA, mMain, mExp;
	int32_t          ex, n;
	const struct branch_info *e;

	assert(cas_mem_init(&s, &test_mem) == 0);
	branches_init(&b, &s);

	/* main @ mA. */
	a[0].id = 1; a[0].kind = 1; a[0].hash = put_blob(&s, "sceneA");
	a[1].id = 2; a[1].kind = 7; a[1].hash = put_blob(&s, "assetA");
	mA = put_manifest(&s, a, 2);
	assert(branches_commit(&b, mA) == 0);

	/* Fork "experiment" off snapshot mA (base recorded, HEAD unchanged). */
	ex = branches_create(&b, "experiment", mA, mA);
	assert(ex == 1);
	assert(branches_count(&b) == 2);
	assert(branches_active(&b) == 0);
	e = branches_get(&b, 1);
	assert(strcmp(e->name, "experiment") == 0);
	assert(e->manifest == mA && e->base == mA);

	/* Advance main to mMain (edit asset id 2). */
	mainB[0] = a[0];
	mainB[1].id = 2; mainB[1].kind = 7; mainB[1].hash = put_blob(&s, "assetMain");
	mMain = put_manifest(&s, mainB, 2);
	assert(branches_commit(&b, mMain) == 0);   /* HEAD == main */
	assert(branches_get(&b, 0)->manifest == mMain);
	assert(branches_get(&b, 1)->manifest == mA);   /* experiment untouched */

	/* Switch to experiment and diverge it to mExp. */
	assert(branches_set_active(&b, ex) == 0);
	assert(branches_active(&b) == 1);
	expB[0] = a[0];
	expB[1].id = 2; expB[1].kind = 7; expB[1].hash = put_blob(&s, "assetExp");
	mExp = put_manifest(&s, expB, 2);
	assert(branches_commit(&b, mExp) == 1);        /* advances experiment only */
	assert(branches_get(&b, 1)->manifest == mExp);
	assert(branches_get(&b, 0)->manifest == mMain);   /* main still diverged */

	/* Working set on experiment resolves experiment's edit. */
	n = branches_working_set(&b, branches_active(&b), ws, 8);
	assert(n == 2);
	assert(ws[0].id == 1 && ws[0].hash == a[0].hash);      /* shared scene */
	assert(ws[1].id == 2 && ws[1].hash == expB[1].hash);   /* experiment asset */

	/* Switch back to main: working set now resolves main's edit instead. */
	assert(branches_set_active(&b, 0) == 0);
	n = branches_working_set(&b, 0, ws, 8);
	assert(n == 2);
	assert(ws[1].hash == mainB[1].hash);
	assert(ws[1].hash != expB[1].hash);

	cas_mem_shutdown(&s);
	printf("PASS: fork off snapshot + switch swaps working set\n");
}

/* main is reserved; names are unique and non-empty; find resolves by name. */
static void test_name_rules(void)
{
	struct cas       s;
	struct branches  b;
	struct cas_entry a;
	cas_hash_t       mA;

	assert(cas_mem_init(&s, &test_mem) == 0);
	branches_init(&b, &s);

	a.id = 1; a.kind = 1; a.hash = put_blob(&s, "x");
	mA = put_manifest(&s, &a, 1);
	assert(branches_commit(&b, mA) == 0);              /* main */
	assert(branches_create(&b, "dev", mA, mA) == 1);

	assert(branches_create(&b, "main", mA, CAS_HASH_NONE) == BRANCH_NONE);
	assert(branches_create(&b, "dev", mA, CAS_HASH_NONE) == BRANCH_NONE);
	assert(branches_create(&b, "", mA, CAS_HASH_NONE) == BRANCH_NONE);

	assert(branches_find(&b, "main") == 0);
	assert(branches_find(&b, "dev") == 1);
	assert(branches_find(&b, "nope") == BRANCH_NONE);

	cas_mem_shutdown(&s);
	printf("PASS: name rules (reserved/unique/non-empty) + find\n");
}

/* set_active validates the index and leaves HEAD unchanged on rejection. */
static void test_set_active_bounds(void)
{
	struct cas       s;
	struct branches  b;
	struct cas_entry a;
	cas_hash_t       mA;

	assert(cas_mem_init(&s, &test_mem) == 0);
	branches_init(&b, &s);

	a.id = 1; a.kind = 1; a.hash = put_blob(&s, "x");
	mA = put_manifest(&s, &a, 1);
	assert(branches_commit(&b, mA) == 0);

	assert(branches_set_active(&b, 5) == -1);
	assert(branches_set_active(&b, -1) == -1);
	assert(branches_active(&b) == 0);          /* unchanged */
	assert(branches_get(&b, 5) == NULL);
	assert(branches_working_set(&b, 5, NULL, 0) == -1);

	cas_mem_shutdown(&s);
	printf("PASS: set_active/get bounds\n");
}

int main(void)
{
	test_bootstrap();
	test_live_save_advance();
	test_fork_and_switch();
	test_name_rules();
	test_set_active_bounds();

	printf("branch tests passed\n");
	return 0;
}
