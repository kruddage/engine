/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "branch_host.h"
#include "branch_serialize.h"
#include "branch_ingest.h"
#include "branch_manifest.h"

#include "branch.h"
#include "snapshot.h"
#include "cas.h"
#include "cas_mem.h"

#include <stddef.h>

/*
 * Debounce window, in frames, between the last edit and an auto-snapshot.  At
 * ~60 fps this is ~2 s of quiescence — long enough to coalesce a burst of edits
 * into one manifest advance + one snapshot, short enough to feel durable.
 */
#define BRANCH_DEBOUNCE_TICKS 120

static struct {
	struct cas               store;
	struct branches          branches;
	struct snapshots         snaps;
	struct subsystem_manager *mgr;
	uint32_t                  label_seq;   /* opaque snapshot tag source */
	int32_t                   debounce;    /* ticks until flush, <0 = idle */
	int32_t                   dirty;       /* an edit is pending capture */
	int32_t                   ready;       /* init succeeded */
} g;

/* ------------------------------------------------------------------ */
/* Flush: capture -> commit -> auto-snapshot                           */
/* ------------------------------------------------------------------ */

static void branch_host_flush(void)
{
	cas_hash_t h;
	int32_t    active;

	g.dirty    = 0;
	g.debounce = -1;

	if (branch_serialize_capture(&g.store, g.mgr, &h) != 0)
		return;

	/* Bootstraps `main` from the first save; advances it CoW thereafter. */
	active = branches_commit(&g.branches, h);
	if (active < 0)
		return;

	snapshots_capture(&g.snaps, &g.branches, active, g.label_seq++);
}

/* ------------------------------------------------------------------ */
/* branch_api vtable                                                   */
/* ------------------------------------------------------------------ */

static uint32_t api_branch_count(void)
{
	return branches_count(&g.branches);
}

static int32_t api_branch_active(void)
{
	return branches_active(&g.branches);
}

static int32_t api_branch_get(int32_t index, struct branch_api_desc *out)
{
	const struct branch_info *bi = branches_get(&g.branches, index);
	uint32_t                  i;

	if (!bi || !out)
		return -1;

	out->index    = index;
	out->active   = (index == branches_active(&g.branches));
	out->has_base = (bi->base != CAS_HASH_NONE);
	for (i = 0; i + 1 < BRANCH_API_NAME_MAX && bi->name[i]; i++)
		out->name[i] = bi->name[i];
	out->name[i] = '\0';
	return 0;
}

static int32_t api_branch_fork(const char *name, int32_t from_snapshot)
{
	int32_t                   active = branches_active(&g.branches);
	cas_hash_t                manifest;
	cas_hash_t                base;

	if (from_snapshot == BRANCH_FROM_HEAD) {
		const struct branch_info *bi = branches_get(&g.branches, active);

		if (!bi)
			return -1;
		manifest = bi->manifest;
		base     = CAS_HASH_NONE;
	} else {
		const struct snapshot_info *si =
			snapshots_get(&g.snaps, active, (uint32_t)from_snapshot);

		if (!si)
			return -1;
		manifest = si->manifest;
		base     = si->manifest;
	}

	return branches_create(&g.branches, name, manifest, base);
}

static int32_t api_branch_switch(int32_t index)
{
	const struct branch_info *bi = branches_get(&g.branches, index);

	if (!bi)
		return -1;
	/* Ingest the target's working set first; leave HEAD put on failure. */
	if (branch_ingest_apply(&g.store, g.mgr, bi->manifest) != 0)
		return -1;
	return branches_set_active(&g.branches, index);
}

static uint32_t api_snapshot_count(void)
{
	return snapshots_count(&g.snaps, branches_active(&g.branches));
}

static int32_t api_snapshot_get(uint32_t i, struct branch_api_snapshot *out)
{
	int32_t                     active = branches_active(&g.branches);
	const struct snapshot_info *si = snapshots_get(&g.snaps, active, i);

	if (!si || !out)
		return -1;
	out->index = i;
	out->label = si->label;
	out->seq   = si->seq;
	return 0;
}

static int32_t api_snapshot_restore(uint32_t i)
{
	int32_t                   active = branches_active(&g.branches);
	const struct branch_info *bi;

	if (snapshots_restore(&g.snaps, &g.branches, active, i,
			      g.label_seq++) != 0)
		return -1;

	bi = branches_get(&g.branches, active);
	if (!bi)
		return -1;
	return branch_ingest_apply(&g.store, g.mgr, bi->manifest);
}

static void api_mark_dirty(void)
{
	g.dirty    = 1;
	g.debounce = BRANCH_DEBOUNCE_TICKS;
}

static int32_t api_merge_supported(void)
{
	return 0;   /* v1: declared but unimplemented (UI shows "coming soon"). */
}

static const struct branch_api g_api = {
	.branch_count     = api_branch_count,
	.branch_active    = api_branch_active,
	.branch_get       = api_branch_get,
	.branch_fork      = api_branch_fork,
	.branch_switch    = api_branch_switch,
	.snapshot_count   = api_snapshot_count,
	.snapshot_get     = api_snapshot_get,
	.snapshot_restore = api_snapshot_restore,
	.mark_dirty       = api_mark_dirty,
	.merge_supported  = api_merge_supported,
};

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

int32_t branch_host_init(struct subsystem_manager *mgr,
			 const struct memory_api *mem)
{
	if (g.ready)
		return 0;
	if (!mem)
		return -1;

	if (cas_mem_init(&g.store, mem) != 0)
		return -1;

	branches_init(&g.branches, &g.store);
	snapshots_init(&g.snaps);

	g.mgr       = mgr;
	g.label_seq = 1;
	g.debounce  = -1;
	g.dirty     = 0;
	g.ready     = 1;
	return 0;
}

void branch_host_shutdown(void)
{
	if (!g.ready)
		return;
	cas_mem_shutdown(&g.store);
	g.ready = 0;
	g.mgr   = NULL;
}

const struct branch_api *branch_host_api(void)
{
	return g.ready ? &g_api : NULL;
}

void branch_host_tick(void)
{
	if (!g.ready || !g.dirty)
		return;
	if (g.debounce > 0)
		g.debounce--;
	if (g.debounce == 0)
		branch_host_flush();
}
