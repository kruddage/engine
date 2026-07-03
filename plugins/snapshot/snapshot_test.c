/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "snapshot.h"
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

/* Big fixed-size aggregates: keep them off the stack. */
static struct branches g_b;
static struct snapshots g_s;

static cas_hash_t asset_manifest(struct cas *s, uint32_t id, const char *bytes)
{
	struct cas_entry e;
	cas_hash_t       m = 0;

	e.id = id;
	e.kind = 7;
	assert(cas_put_blob(s, bytes, (uint32_t)strlen(bytes), &e.hash) == 0);
	assert(cas_put_manifest(s, &e, 1, &m) == 0);
	return m;
}

/* Snapshots append in time order; an unchanged state is not re-snapshotted. */
static void test_capture_timeline(void)
{
	struct cas s;
	cas_hash_t mA, mB;

	assert(cas_mem_init(&s, &test_mem) == 0);
	branches_init(&g_b, &s);
	snapshots_init(&g_s);

	mA = asset_manifest(&s, 1, "stateA");
	assert(branches_commit(&g_b, mA) == 0);          /* main @ mA */

	assert(snapshots_capture(&g_s, &g_b, 0, 100u) == 0);
	assert(snapshots_count(&g_s, 0) == 1u);
	assert(snapshots_get(&g_s, 0, 0)->manifest == mA);

	mB = asset_manifest(&s, 1, "stateB");
	assert(branches_commit(&g_b, mB) == 0);          /* advance main */
	assert(snapshots_capture(&g_s, &g_b, 0, 200u) == 1);
	assert(snapshots_count(&g_s, 0) == 2u);
	assert(snapshots_get(&g_s, 0, 0)->manifest == mA);   /* order preserved */
	assert(snapshots_get(&g_s, 0, 1)->manifest == mB);

	/* Capturing again with no change is an idle no-op. */
	assert(snapshots_capture(&g_s, &g_b, 0, 300u) == 1);
	assert(snapshots_count(&g_s, 0) == 2u);

	cas_mem_shutdown(&s);
	printf("PASS: capture timeline (ordered, dedup on unchanged)\n");
}

/* A snapshot costs no content blob — capturing does not grow storage. */
static void test_cow_cheap(void)
{
	struct cas s;
	cas_hash_t mA;
	uint32_t   before, after;
	int        k;

	assert(cas_mem_init(&s, &test_mem) == 0);
	branches_init(&g_b, &s);
	snapshots_init(&g_s);

	mA = asset_manifest(&s, 1, "stateA");
	assert(branches_commit(&g_b, mA) == 0);
	before = s.backing.count(s.backing.ctx);

	for (k = 0; k < 10; k++)
		(void)snapshots_capture(&g_s, &g_b, 0, (uint32_t)k);

	after = s.backing.count(s.backing.ctx);
	assert(after == before);   /* snapshots reference, never copy */

	cas_mem_shutdown(&s);
	printf("PASS: snapshots are copy-on-write cheap (no new blobs)\n");
}

/*
 * Restore rolls the branch back to a chosen snapshot and preserves the
 * pre-restore working state as a fresh snapshot (non-destructive).
 */
static void test_restore_nondestructive(void)
{
	struct cas       s;
	cas_hash_t       mA, mB, mC;
	struct cas_entry ws[4];
	int32_t          n;

	assert(cas_mem_init(&s, &test_mem) == 0);
	branches_init(&g_b, &s);
	snapshots_init(&g_s);

	mA = asset_manifest(&s, 1, "stateA");
	assert(branches_commit(&g_b, mA) == 0);
	assert(snapshots_capture(&g_s, &g_b, 0, 1u) == 0);   /* snap 0 = mA */

	mB = asset_manifest(&s, 1, "stateB");
	assert(branches_commit(&g_b, mB) == 0);
	assert(snapshots_capture(&g_s, &g_b, 0, 2u) == 1);   /* snap 1 = mB */

	/* Diverge the working state to mC WITHOUT snapshotting it. */
	mC = asset_manifest(&s, 1, "stateC");
	assert(branches_commit(&g_b, mC) == 0);
	assert(snapshots_count(&g_s, 0) == 2u);

	/* Restore to snapshot 0 (mA): the unsnapshotted mC must be preserved. */
	assert(snapshots_restore(&g_s, &g_b, 0, 0, 9u) == 0);
	assert(snapshots_count(&g_s, 0) == 3u);
	assert(snapshots_get(&g_s, 0, 2)->manifest == mC);   /* pre-restore saved */
	assert(branches_get(&g_b, 0)->manifest == mA);       /* rolled back */

	/* The working set now resolves the restored state (mA's content). */
	n = branches_working_set(&g_b, 0, ws, 4);
	assert(n == 1);
	assert(ws[0].id == 1);
	assert(ws[0].hash == cas_hash("stateA", 6));

	cas_mem_shutdown(&s);
	printf("PASS: restore is non-destructive (pre-restore state kept)\n");
}

/* A snapshot doubles as a fork base for a new branch. */
static void test_snapshot_as_fork_base(void)
{
	struct cas       s;
	cas_hash_t       mA, mB;
	const struct snapshot_info *snap;
	int32_t          ex, n;
	struct cas_entry ws[4];

	assert(cas_mem_init(&s, &test_mem) == 0);
	branches_init(&g_b, &s);
	snapshots_init(&g_s);

	mA = asset_manifest(&s, 1, "baseState");
	assert(branches_commit(&g_b, mA) == 0);
	assert(snapshots_capture(&g_s, &g_b, 0, 1u) == 0);
	mB = asset_manifest(&s, 1, "movedOn");
	assert(branches_commit(&g_b, mB) == 0);   /* main has moved past the snapshot */

	/* Fork a branch off the snapshot (not head). */
	snap = snapshots_get(&g_s, 0, 0);
	assert(snap->manifest == mA);
	ex = branches_create(&g_b, "fromSnap", snap->manifest, snap->manifest);
	assert(ex == 1);
	assert(branches_get(&g_b, 1)->base == mA);       /* base recorded */
	assert(branches_get(&g_b, 1)->manifest == mA);   /* starts at the snapshot */

	n = branches_working_set(&g_b, ex, ws, 4);
	assert(n == 1 && ws[0].id == 1);

	cas_mem_shutdown(&s);
	printf("PASS: snapshot doubles as a fork base\n");
}

/* Bad branch / index are handled without crashing. */
static void test_bounds(void)
{
	struct cas s;
	cas_hash_t mA;

	assert(cas_mem_init(&s, &test_mem) == 0);
	branches_init(&g_b, &s);
	snapshots_init(&g_s);

	mA = asset_manifest(&s, 1, "x");
	assert(branches_commit(&g_b, mA) == 0);

	assert(snapshots_capture(&g_s, &g_b, 5, 0u) == -1);   /* no such branch */
	assert(snapshots_count(&g_s, 5) == 0u);
	assert(snapshots_get(&g_s, 0, 0) == NULL);            /* empty timeline */
	assert(snapshots_restore(&g_s, &g_b, 0, 0, 0u) == -1);/* nothing to restore */

	cas_mem_shutdown(&s);
	printf("PASS: bounds handled\n");
}

int main(void)
{
	test_capture_timeline();
	test_cow_cheap();
	test_restore_nondestructive();
	test_snapshot_as_fork_base();
	test_bounds();

	printf("snapshot tests passed\n");
	return 0;
}
