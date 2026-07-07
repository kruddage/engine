/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "snapshot.h"

#include <stddef.h>

void snapshots_init(struct snapshots *s)
{
	uint32_t i;

	for (i = 0; i < BRANCH_MAX; i++) {
		s->lanes[i].count    = 0;
		s->lanes[i].next_seq = 0;
	}
}

static struct snapshot_timeline *lane(struct snapshots *s, int32_t branch)
{
	if (branch < 0 || (uint32_t)branch >= BRANCH_MAX)
		return NULL;
	return &s->lanes[branch];
}

int32_t snapshots_capture(struct snapshots *s, struct branches *b,
			  int32_t branch, uint32_t label)
{
	struct snapshot_timeline *tl = lane(s, branch);
	const struct branch_info *bi = branches_get(b, branch);
	struct snapshot_info     *si;

	if (!tl || !bi)
		return -1;

	/* Idle no-op: an unchanged working state adds no new snapshot. */
	if (tl->count > 0
	    && tl->list[tl->count - 1].manifest == bi->manifest)
		return (int32_t)(tl->count - 1);

	if (tl->count >= SNAPSHOT_MAX_PER_BRANCH)
		return -1;

	si = &tl->list[tl->count];
	si->manifest = bi->manifest;
	si->label    = label;
	si->seq      = tl->next_seq++;
	return (int32_t)tl->count++;
}

uint32_t snapshots_count(const struct snapshots *s, int32_t branch)
{
	if (branch < 0 || (uint32_t)branch >= BRANCH_MAX)
		return 0;
	return s->lanes[branch].count;
}

const struct snapshot_info *snapshots_get(const struct snapshots *s,
					  int32_t branch, uint32_t i)
{
	if (branch < 0 || (uint32_t)branch >= BRANCH_MAX)
		return NULL;
	if (i >= s->lanes[branch].count)
		return NULL;
	return &s->lanes[branch].list[i];
}

int32_t snapshots_restore(struct snapshots *s, struct branches *b,
			  int32_t branch, uint32_t i, uint32_t label)
{
	const struct snapshot_info *snap = snapshots_get(s, branch, i);
	cas_hash_t                  target;

	if (!snap)
		return -1;
	target = snap->manifest;   /* read before capture may append */

	/* Non-destructive: preserve the pre-restore working state first. */
	if (snapshots_capture(s, b, branch, label) < 0)
		return -1;

	return branches_set_manifest(b, branch, target);
}
