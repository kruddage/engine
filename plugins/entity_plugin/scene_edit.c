/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "scene_edit.h"

#include <stddef.h>

/*
 * Opaque command state: the world to swap, the allocator that owns the
 * snapshots, and the two snapshots that bracket the edit. apply/redo ingests
 * `after`, revert/undo ingests `before`. The edit service owns this memento's
 * lifetime and calls free when it drops the entry.
 */
struct scene_edit_memento {
	struct world            *w;
	const struct memory_api *mem;
	struct world_snapshot   *before;
	struct world_snapshot   *after;
};

static void scene_edit_apply(void *memento)
{
	struct scene_edit_memento *m = memento;

	world_snapshot_restore(m->w, m->after);
}

static void scene_edit_revert(void *memento)
{
	struct scene_edit_memento *m = memento;

	world_snapshot_restore(m->w, m->before);
}

static void scene_edit_free(void *memento)
{
	struct scene_edit_memento *m = memento;

	if (!m)
		return;
	world_snapshot_free(m->before, m->mem);
	world_snapshot_free(m->after, m->mem);
	m->mem->free(m);
}

uint32_t scene_edit_key(int32_t id, enum scene_edit_field field)
{
	if (field == SCENE_EDIT_NONE || id < 0)
		return 0;
	return ((uint32_t)id << 2) | (uint32_t)field;
}

void scene_edit_record(const struct edit_api *edit, const struct memory_api *mem,
		       struct world *w, struct world_snapshot *before,
		       const char *label, uint32_t coalesce_key)
{
	struct scene_edit_memento *m;
	struct world_snapshot     *after;
	struct edit_cmd            cmd;

	/* No history/allocator (or nothing captured): drop before and go. */
	if (!edit || !mem || !before) {
		world_snapshot_free(before, mem);
		return;
	}

	after = world_snapshot_capture(w, mem);
	m     = mem->alloc(sizeof(*m));
	if (!after || !m) {
		/* The mutation already happened; we just can't record it. */
		world_snapshot_free(before, mem);
		world_snapshot_free(after, mem);
		mem->free(m);
		return;
	}

	m->w      = w;
	m->mem    = mem;
	m->before = before;
	m->after  = after;

	cmd.apply        = scene_edit_apply;
	cmd.revert       = scene_edit_revert;
	cmd.free         = scene_edit_free;
	cmd.memento      = m;
	cmd.coalesce_key = coalesce_key;
	cmd.label        = label;

	/* push() re-applies `after` (already the live state) — a no-op — then
	 * records the entry. */
	edit->push(&cmd);
}
