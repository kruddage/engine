/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BRANCH_API_H
#define BRANCH_API_H

#include <stdint.h>

/*
 * Branching capability seam (#213/#217) — the runtime face of the branch model
 * (#215) and auto-snapshots (#216) built over the content-addressed store
 * (#214).  This is what the editor UI and the live-save wiring drive; it hides
 * IndexedDB, the cas store, and the world/catalog plumbing behind a plain
 * vtable.
 *
 * Obtain it through the backend abstraction (#189), gated by
 * BACKEND_CAP_BRANCHING:
 *
 *   const struct backend_api *be = subsystem_manager_get_api(mgr, "backend");
 *   const struct branch_api  *br =
 *           (be->get_caps() & BACKEND_CAP_BRANCHING) ? be->branching() : NULL;
 *
 * br is NULL when the active provider has no branching (e.g. a future remote
 * provider without it) — every consumer must degrade safely when it is absent.
 *
 * The model, restated for callers: a branch IS a live whole-project state
 * ("HEAD + my diffs"), not a commit log.  Switching hands the engine that
 * branch's working set and re-ingests the world + catalog in one shot (atomic
 * reload).  Snapshots are per-branch restore points and double as fork bases.
 * Merge is declared but unimplemented in v1 — merge_supported() returns 0.
 */

#define BRANCH_API_NAME_MAX 48u   /* mirrors BRANCH_NAME_MAX incl. NUL */

/* From-snapshot sentinel for fork(): fork from current head, not a snapshot. */
#define BRANCH_FROM_HEAD (-1)

struct branch_api_desc {
	int32_t  index;                      /* branch index (stable handle) */
	char     name[BRANCH_API_NAME_MAX];  /* branch name, NUL-terminated */
	int32_t  active;                     /* nonzero iff this branch is HEAD */
	int32_t  has_base;                   /* nonzero iff forked from a snapshot */
};

struct branch_api_snapshot {
	uint32_t index;   /* position in the active branch's timeline (0..count) */
	uint32_t label;   /* opaque caller tag at capture (e.g. a timestamp) */
	uint32_t seq;     /* monotonic capture order within the timeline */
};

struct branch_api {
	/* ---- enumerate branches ------------------------------------------ */

	/* Number of branches (0 before the first save bootstraps `main`). */
	uint32_t (*branch_count)(void);
	/* HEAD's branch index, or BRANCH_FROM_HEAD (-1) before bootstrap. */
	int32_t  (*branch_active)(void);
	/* Fill *out for branch `index`.  Returns 0, or -1 on a bad index. */
	int32_t  (*branch_get)(int32_t index, struct branch_api_desc *out);

	/* ---- mutate branches --------------------------------------------- */

	/*
	 * Fork a new branch named `name`.  from_snapshot is an index on the
	 * ACTIVE branch's snapshot timeline to fork from, or BRANCH_FROM_HEAD to
	 * fork from current head.  Does not switch HEAD.  Returns the new branch
	 * index, or -1 (reserved/duplicate/empty name, bad snapshot, or full).
	 */
	int32_t  (*branch_fork)(const char *name, int32_t from_snapshot);
	/*
	 * Switch HEAD to `index` and re-ingest that branch's working set — the
	 * world swaps via world_ingest_scene and the catalog swaps with it, in
	 * one atomic reload.  Returns 0 on success, -1 on a bad index or an
	 * ingest failure (HEAD is left unchanged on failure).
	 */
	int32_t  (*branch_switch)(int32_t index);

	/* ---- snapshots of the active branch ------------------------------ */

	/* Number of snapshots on the active branch's timeline. */
	uint32_t (*snapshot_count)(void);
	/* Fill *out for snapshot `i` (time-ordered).  Returns 0, or -1. */
	int32_t  (*snapshot_get)(uint32_t i, struct branch_api_snapshot *out);
	/*
	 * Restore the active branch's working state to snapshot `i` and re-ingest
	 * (atomic reload).  Non-destructive: the pre-restore state is snapshotted
	 * first.  Returns 0 on success, -1 on a bad index or ingest failure.
	 */
	int32_t  (*snapshot_restore)(uint32_t i);

	/* ---- live-save --------------------------------------------------- */

	/*
	 * The runtime signals that the project changed (a scene or asset edit).
	 * The backend debounces these and, on quiescence, advances the active
	 * branch copy-on-write (branches_commit) and captures an auto-snapshot.
	 * Cheap to call per mutation; coalesced internally.
	 */
	void     (*mark_dirty)(void);

	/* ---- merge seam (declared, unimplemented in v1) ------------------ */

	/* Always 0 in v1 — the UI shows a disabled "Merge… (coming soon)". */
	int32_t  (*merge_supported)(void);
};

#endif /* BRANCH_API_H */
