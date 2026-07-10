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
	/*
	 * Bind id to a material by asset id (sets COMPONENT_MATERIAL); a zero
	 * material_ref unbinds, clearing COMPONENT_MATERIAL. Mirrors
	 * set_render_ref, but for the material asset that tints the draw
	 * rather than the mesh it draws.
	 */
	void    (*set_material_ref)(int32_t id, uint32_t material_ref);
	/*
	 * Bind id to a behavior script by asset id (sets COMPONENT_SCRIPT); a
	 * zero script_ref unbinds, clearing COMPONENT_SCRIPT. Mirrors
	 * set_render_ref / set_material_ref, but for the .kscm script asset that
	 * drives the entity each tick (move, scale, rotate) rather than the mesh
	 * it draws or the material that tints it.
	 */
	void    (*set_script_ref)(int32_t id, uint32_t script_ref);

	/* Shared selection: -1 = none. set ignores stale/out-of-range ids. */
	int32_t (*get_selected)(void);
	void    (*set_selected)(int32_t id);

	/*
	 * Simulation mode: paused skips world_tick + entity scripts for the
	 * frame (the editor's "Paused" state), so the scene holds still while
	 * everything else — rendering, gizmo, undo/redo — keeps running.
	 * Defaults to false (playing).
	 */
	int32_t (*get_paused)(void);
	void    (*set_paused)(int32_t paused);
};

#endif /* ENTITY_API_H */
