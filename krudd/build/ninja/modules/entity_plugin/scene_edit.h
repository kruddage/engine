/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SCENE_EDIT_H
#define SCENE_EDIT_H

#include "world.h"
#include "edit_api.h"
#include "memory_api.h"

#include <stdint.h>

/*
 * Records runtime entity edits onto the "edit" history as snapshot-based
 * commands: the memento carries a before and after world_snapshot, so undo
 * ingests the before and redo the after. Because a snapshot is whole-world,
 * the hard cases (destroy cascade, selection) reverse for free.
 *
 * The scene plugin owns the calling convention: capture a before-snapshot,
 * mutate the world, then scene_edit_record() the change. All of this is a
 * no-op when the "edit" service is absent, so the scene api keeps working on
 * an engine build without undo.
 */

/* Field tags: consecutive same-entity, same-field edits coalesce into one
 * history entry (a continuous gizmo drag), so use a per-field tag. */
enum scene_edit_field {
	SCENE_EDIT_NONE      = 0,	/* never coalesce — create / destroy */
	SCENE_EDIT_TRANSFORM = 1,
	SCENE_EDIT_NAME      = 2,
	SCENE_EDIT_RENDER    = 3,	/* mesh (render_ref) rebind */
	SCENE_EDIT_MATERIAL  = 4,	/* material (material_ref) rebind */
	SCENE_EDIT_SCRIPT    = 5,	/* script (script_ref) rebind */
};

/* Compose an entity id and field tag into a non-zero coalesce key (0 when the
 * field is NONE or the id is invalid, i.e. "never coalesce"). */
uint32_t scene_edit_key(int32_t id, enum scene_edit_field field);

/*
 * Record a completed mutation. Captures the after-snapshot, pairs it with the
 * caller's before-snapshot into a memento, and pushes an edit_cmd. Takes
 * ownership of `before` and frees it on every path — including when `edit` or
 * `before` is NULL, or on allocation failure — so callers never double-free.
 * All allocation goes through `mem` (the same allocator that captured
 * `before`). `label` must outlive the history entry (a string literal).
 */
void scene_edit_record(const struct edit_api *edit, const struct memory_api *mem,
		       struct world *w, struct world_snapshot *before,
		       const char *label, uint32_t coalesce_key);

#endif /* SCENE_EDIT_H */
