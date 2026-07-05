/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ENTITY_API_H
#define ENTITY_API_H

#include "world.h"

#include <stdint.h>

/*
 * Cross-plugin access to the live entity world, published as the "scene"
 * subsystem api:
 *
 *   const struct entity_api *e = subsystem_manager_get_api(mgr, "scene");
 *   const struct world      *w = e->get_world();
 *
 * A renderer walks w->count entities, skips tombstones (alive[i] == 0), tests
 * each mask for COMPONENT_RENDER, and draws using world_xform[i] together with
 * render_ref[i].
 *
 * Beyond reading the world, editor tools drive it through the runtime mutators
 * and the shared selection model below: drag-to-spawn is create_entity, the
 * property panel and gizmo are set_transform / set_name / set_render_ref, and
 * all agree on the active entity via get_selected / set_selected.
 */
struct entity_api {
	const struct world *(*get_world)(void);
	/* Load and ingest a .scene asset by path; 0 on success, -1 otherwise. */
	int32_t             (*load_scene)(const char *path);

	/*
	 * Append an entity under parent (-1 = root) with the given local
	 * transform and component mask; render_ref is applied (and
	 * COMPONENT_RENDER set) when non-zero. Returns the new id, or -1 if the
	 * world is full or parent is not a live entity.
	 */
	int32_t (*create_entity)(int32_t parent, const struct transform *local,
				 uint32_t mask, uint32_t render_ref);
	/* Tombstone id and its subtree; clears a selection it tombstones. */
	void    (*destroy_entity)(int32_t id);
	/* Overwrite id's local transform (visible after the next tick). */
	void    (*set_transform)(int32_t id, const struct transform *local);
	/* Rename id; NULL/empty clears its name. */
	void    (*set_name)(int32_t id, const char *name);
	/*
	 * Bind id to a mesh by asset id (sets COMPONENT_RENDER); a zero
	 * render_ref unbinds, clearing COMPONENT_RENDER. This is the editable
	 * counterpart to the render_ref that create_entity / drag-to-spawn set.
	 */
	void    (*set_render_ref)(int32_t id, uint32_t render_ref);

	/* Shared selection: -1 = none. set ignores stale/out-of-range ids. */
	int32_t (*get_selected)(void);
	void    (*set_selected)(int32_t id);

	/*
	 * Serialize the live world to canonical .scene v1 bytes (#235) — the
	 * whole-project scene the branching store content-addresses (#214).
	 * Encodes via world_export_scene + the registered scene encoder and
	 * writes the size to *out_size.  Returns a freshly allocated buffer the
	 * caller owns (free via the engine allocator), or NULL on failure.  An
	 * empty world yields a valid zero-entity scene, never NULL-for-empty.
	 */
	void   *(*export_scene_bytes)(uint32_t *out_size);

	/*
	 * Replace the live world with the scene decoded from `bytes` — the atomic
	 * reload behind a branch switch / snapshot restore (#215/#216).  Decodes
	 * via the registered scene decoder and world_ingest_scene, swapping the
	 * world in one shot.  Returns 0 on success, -1 on a decode/ingest failure
	 * (the live world is left unchanged on failure).
	 */
	int32_t (*ingest_scene_bytes)(const void *bytes, uint32_t size);
};

#endif /* ENTITY_API_H */
