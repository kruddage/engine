/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SNAPSHOT_H
#define SNAPSHOT_H

#include "branch.h"
#include "cas.h"

#include <stdint.h>

/*
 * Per-branch auto-snapshots + restore (#216).  A snapshot is a frozen manifest
 * hash on a branch's timeline — the durable restore point the live-save model
 * needs.  Because storage is copy-on-write (#214), a snapshot costs only a
 * manifest reference (no content copy), so snapshots can be frequent and are
 * kept all for v1 (pruning is out of scope).
 *
 * Capture is debounced on change in the runtime; at this layer capture also
 * dedups against the latest snapshot, so an unchanged working state never adds
 * a new snapshot ("idle produces no new snapshots").  Snapshots double as fork
 * bases: forking a branch off a snapshot passes that snapshot's manifest to
 * branches_create (#215).  Pure logic over struct branches, native-testable.
 */

/* Keep-all bound per branch for v1 (pruning deferred; revisit only if it bites). */
#define SNAPSHOT_MAX_PER_BRANCH 256u

struct snapshot_info {
	cas_hash_t manifest;   /* the frozen whole-project state */
	uint32_t   label;      /* opaque caller tag (e.g. a timestamp) */
	uint32_t   seq;        /* monotonic capture order within the timeline */
};

struct snapshot_timeline {
	struct snapshot_info list[SNAPSHOT_MAX_PER_BRANCH];
	uint32_t             count;
	uint32_t             next_seq;
};

struct snapshots {
	struct snapshot_timeline lanes[BRANCH_MAX];   /* one timeline per branch */
};

/* Clear every branch's timeline. */
void snapshots_init(struct snapshots *s);

/*
 * Capture a branch's current working manifest as a new snapshot on its
 * timeline, tagged with `label`.  If the working state is unchanged from the
 * most recent snapshot, no snapshot is added and the latest index is returned
 * (idle no-op).  Returns the snapshot index within the timeline, or -1 on a bad
 * branch or a full timeline.
 */
int32_t snapshots_capture(struct snapshots *s, struct branches *b,
			  int32_t branch, uint32_t label);

/* Number of snapshots on a branch's timeline (0 for a bad branch). */
uint32_t snapshots_count(const struct snapshots *s, int32_t branch);

/* Borrow snapshot i (time-ordered) on a branch's timeline, or NULL. */
const struct snapshot_info *snapshots_get(const struct snapshots *s,
					  int32_t branch, uint32_t i);

/*
 * Restore a branch's working state to snapshot i on its timeline.  Non-
 * destructive: the pre-restore working state is captured first (tagged
 * `label`), so a restore can itself be rolled back.  The caller re-ingests the
 * branch's working set afterward (atomic reload).  Returns 0 on success, -1 on
 * a bad branch/index or a full timeline.
 */
int32_t snapshots_restore(struct snapshots *s, struct branches *b,
			  int32_t branch, uint32_t i, uint32_t label);

#endif /* SNAPSHOT_H */
