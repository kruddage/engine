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
	 * Build a scene from a (scene ...) Scheme form (see core/scene_script.scm),
	 * spawning its entities into the live world. Returns the entity count, or
	 * -1 if the interpreter is down or SRC is not a scene form. This is how a
	 * built-in game boots its world: the scene is authored .scm evaluated
	 * against the shared image — the source-authored twin of load_scene.
	 */
	int32_t             (*build_scene_scm)(const char *src);
	/*
	 * Invoke image function `fn` (an integer -> integer procedure) with `arg`,
	 * the live world bound so image-side game rules can spawn and mutate in
	 * response — the runtime twin of build_scene_scm. Returns fn's integer
	 * result, or -1 if the interpreter is down or fn is undefined. A game
	 * plugin uses this to hand a picked cell to its Scheme rules.
	 */
	int32_t             (*dispatch_scm)(const char *fn, int32_t arg);
	/*
	 * Empty the world — tombstone every entity and clear the selection. The
	 * launcher calls this before building a different scene, so switching
	 * games starts from a clean world rather than layering one over another.
	 */
	void                (*clear_world)(void);

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
	 * rather than the mesh it draws. An entity with no COMPONENT_MATERIAL
	 * keeps its mesh (still hit-testable and collidable) but the renderer
	 * skips drawing it.
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
	/*
	 * Override id's script parameters with `len` tight-packed bytes (the
	 * layout its bound script's params clause reports); len 0 clears the
	 * override. The per-entity value layer over a script's declared params —
	 * the behavior twin of a material instance over a shader's Material
	 * block. Records an undo step; consecutive edits to one entity coalesce,
	 * so a slider drag is a single history entry.
	 */
	void    (*set_script_params)(int32_t id, const uint8_t *bytes,
				     uint32_t len);
	/*
	 * Override id's material parameters with `len` std140 Material-block
	 * bytes (the layout after a material's shader-ref header); len 0 clears
	 * the override. The per-entity value layer over a material's shared
	 * params — what makes two entities on one material asset draw with
	 * different colors. Only the params are per-entity; the shader still
	 * comes from the bound material. Records an undo step; consecutive edits
	 * to one entity coalesce, so a color-swatch drag is a single history
	 * entry.
	 */
	void    (*set_material_params)(int32_t id, const uint8_t *bytes,
				       uint32_t len);
	/*
	 * Override id's mesh parameters with `len` tight-packed bytes (the
	 * layout its bound mesh's params clause reports); len 0 clears the
	 * override. The per-entity value layer over a mesh's declared params —
	 * what makes two entities on one mesh asset draw at different sizes.
	 * Only the params are per-entity; the mesh asset still comes from the
	 * bound render_ref. Records an undo step; consecutive edits to one entity
	 * coalesce, so a slider drag is a single history entry.
	 */
	void    (*set_mesh_params)(int32_t id, const uint8_t *bytes,
				   uint32_t len);
	/*
	 * Override id's texture parameters with `len` tight-packed bytes (the
	 * layout its material's bound texture's params clause reports); len 0
	 * clears the override. The per-entity value layer over a texture's declared
	 * params — what makes two entities on one material bake its texture with
	 * different generation params (a checker at scale 8 vs 16). Only the params
	 * are per-entity; which texture and at what resolution still come from the
	 * material's texture slot. Records an undo step; consecutive edits to one
	 * entity coalesce, so a slider drag is a single history entry.
	 */
	void    (*set_texture_params)(int32_t id, const uint8_t *bytes,
				      uint32_t len);

	/* Shared selection: -1 = none. set ignores stale/out-of-range ids. */
	int32_t (*get_selected)(void);
	void    (*set_selected)(int32_t id);

	/*
	 * Game-driven outline target: the entity the renderer outlines in-game,
	 * independent of the editor selection above. A game's rules set it (the
	 * chess piece the player picked up) so the selection outline shows
	 * outside editor chrome; -1 = none, set ignores stale/out-of-range ids.
	 */
	int32_t (*get_outline)(void);
	void    (*set_outline)(int32_t id);

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
