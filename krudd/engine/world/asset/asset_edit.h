/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ASSET_EDIT_H
#define ASSET_EDIT_H

#include "asset.h"		/* ASSET_PATH_MAX, asset_mut_* / asset_catalog_* */
#include "edit_api.h"
#include "memory_api.h"

#include <stdint.h>

/*
 * Records authored-asset edits onto the "edit" history as memento commands: the
 * memento carries a before and after state (the asset's bytes, path and type by
 * stable id), so undo restores the before and redo the after. A "state" is
 * either absent (the id held nothing) or present, which lets one command shape
 * cover all three mutations:
 *
 *   create   before = absent,        after = present(new bytes)
 *   set_data before = present(old),  after = present(new bytes)
 *   destroy  before = present(old),  after = absent
 *
 * Recreating an absent asset uses asset_mut_inject(), so undo-of-destroy and
 * redo-of-create bring the asset back under its ORIGINAL stable id — references
 * to it survive the round trip. Undo targets the in-editor model only; it never
 * touches the persistence backend (re-saving after an undo persists the reverted
 * bytes). All of this is a no-op when the "edit" service is absent, so asset_mut
 * keeps working on an engine build without undo.
 */

/* Opaque per-id state snapshot: absent, or present with bytes + path + type. */
struct asset_snapshot;

/*
 * Capture the current catalog state of `id`. Returns a "present" snapshot with a
 * private copy of the bytes, or an "absent" snapshot when `id` names nothing.
 * Returns NULL only on allocation failure (or a NULL allocator).
 */
struct asset_snapshot *asset_snapshot_capture(uint32_t id,
					      const struct memory_api *mem);

/* An explicit "nothing here yet" snapshot — the before-state of a create. */
struct asset_snapshot *asset_snapshot_absent(const struct memory_api *mem);

/* Release a snapshot and its byte copy. Tolerates NULL. */
void asset_snapshot_free(struct asset_snapshot *s, const struct memory_api *mem);

/*
 * Compose a non-zero coalesce key for repeated edits to the SAME asset (so a run
 * of saves folds into one history entry). Bit 31 namespaces asset keys away from
 * scene keys — scene entity ids are small world-array indices, so a scene key
 * ((id << 2) | field) never sets bit 31 — which keeps the one shared timeline
 * from ever folding a scene edit into an asset edit that happens to collide.
 * Returns 0 for id 0 (i.e. "never coalesce").
 */
uint32_t asset_edit_key(uint32_t id);

/*
 * Record a completed mutation of `id`: captures the after-state, pairs it with
 * the caller's `before`, and pushes an edit_cmd. Takes ownership of `before` and
 * frees it on every path — including when `edit`, `mem` or `before` is NULL, or
 * on allocation failure — so callers never double-free. All allocation goes
 * through `mem` (the same allocator that captured `before`). `label` must
 * outlive the history entry (a string literal).
 */
void asset_edit_record(const struct edit_api *edit, const struct memory_api *mem,
		       uint32_t id, struct asset_snapshot *before,
		       const char *label, uint32_t coalesce_key);

#endif /* ASSET_EDIT_H */
