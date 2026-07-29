/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WORLD_H
#define WORLD_H

#include "math_types.h"
#include "scene.h"

#include <stdint.h>

/*
 * Runtime entity world — flat struct-of-arrays keyed by a dense entity id.
 * Distinct from the at-rest `struct scene` the .scene decoder returns: the
 * decoder produces a flat transfer struct; the entity system ingests it into
 * these parallel columns (see world_ingest_scene).
 *
 * Hierarchy is the int32_t parent[] column (-1 = root), never pointers.
 * Creation appends and destruction tombstones a slot (and its descendants)
 * without shifting any index, so the parent refs stored in surviving entities
 * stay valid — a naive swap-remove would shift indices and silently corrupt
 * the hierarchy. **An id, once handed out, means that entity forever.**
 *
 * ## Ids are stable; index order is not topological (#950)
 *
 * This used to say the columns were kept in topological order — parent index
 * always below child index — and every hierarchy walk here was one forward
 * sweep on the strength of it. world_set_parent ended that, and it could not
 * have done anything else: reparenting an entity under one that happens to sit
 * at a higher index can preserve the ordering or it can preserve the ids, and
 * it cannot preserve both. Ids win, because the selection, the undo snapshots,
 * the editor's outliner rows and the bridge all name entities by id, and an id
 * that moved under a reparent would silently mean a different entity in every
 * one of them.
 *
 * So `parent[e]` may be any live id, above or below e. What that costs is
 * spelled out at each walk in entity.c — propagation resolves each entity after
 * its ancestors rather than by index, and the destroy cascade and the subtree
 * walks sweep to a fixed point instead of once. What it does not cost is the
 * export contract: world_export_scene still emits parent-before-child, because
 * struct scene's ordering (scene.h) is a file format and stays one.
 *
 * Cycles are the failure this ordering used to make unrepresentable, and
 * world_set_parent is now what makes them so: it refuses a parent that is a
 * descendant of the entity being moved. Every walk below is depth-capped
 * regardless, so a corrupt column degrades rather than spinning.
 *
 * enum component_bit, struct scene, struct scene_entity and SCENE_NO_NAME
 * come from scene.h; the runtime mask mirrors the file mask exactly so ingest
 * stays a near-memcpy. A transform is implicit on every entity (no bit).
 */

/*
 * Per-entity editor flags — hidden and locked (#950).
 *
 * Runtime only, and that is the whole design. They are **not** exported by
 * world_export_scene or scene_save, **not** captured by a snapshot, and **not**
 * recorded on the undo history: hiding a light to see behind it is a property
 * of how someone is looking at a scene, not of the scene, and a hidden flag in
 * the file is how a level ships with something invisible that nobody can find.
 *
 * They live here rather than in the editor because the engine is what draws and
 * what picks. An editor-side "hidden" would dim a row in a list while the
 * entity carried on rendering, which is not hiding it — it is lying about it.
 */
enum world_entity_flag {
	/* Skipped by the renderer and by picking. */
	WORLD_ENTITY_HIDDEN = 1u << 0,
	/* Skipped by picking, so a click cannot land a gizmo on it. */
	WORLD_ENTITY_LOCKED = 1u << 1,
};

#define WORLD_MAX_ENTITIES 4096
#define WORLD_NAME_BYTES   4096
#define WORLD_NO_PARENT    (-1)
#define WORLD_NO_NAME      SCENE_NO_NAME

/*
 * Per-entity script-parameter override: the tight-packed values a scripted
 * entity overrides on its bound script's params clause (the CPU-side twin of a
 * material instance overriding a shader's Material block). Held inline per
 * entity — small, bounded, and rewritten in place as a slider drags — rather
 * than in an append-only blob like names. A len of 0 means "no override": the
 * script runs on its param defaults.
 */
#define WORLD_SCRIPT_PARAM_CAP 64

/*
 * Per-entity material-parameter override: the material instance the script
 * comment above only alludes to, now real. These are the std140 Material block
 * bytes (everything after a material's shader-ref header — the same layout the
 * renderer uploads to the Material UBO) that this entity substitutes for its
 * bound material asset's, letting two entities share one material asset yet draw
 * with different colors. Held inline like the script override; a len of 0 means
 * "no override": the entity draws with its material asset's own params. Sized to
 * the renderer's Material UBO cap so any valid block fits without truncation.
 */
#define WORLD_MATERIAL_PARAM_CAP 256

/*
 * Per-entity mesh-parameter override: the tight-packed values a rendered entity
 * substitutes on its bound mesh's params clause, letting two entities share one
 * mesh asset yet draw at different sizes — the geometry twin of the material
 * override above. Held inline like the others; a len of 0 means "no override":
 * the mesh generates on its param defaults. Tight-packed (no std140 padding),
 * since these feed a CPU geometry generator, not a GPU uniform block; sized like
 * the script override, as mesh params are typically a handful of scalars.
 */
#define WORLD_MESH_PARAM_CAP 64

/*
 * Per-entity texture-parameter override: the tight-packed values a rendered
 * entity substitutes on its material's bound texture's params clause, letting two
 * entities share one material yet bake its procedural texture with different
 * generation params — a checker at scale 8 vs 16 — the pixel twin of the mesh
 * override above. Held inline like the others; a len of 0 means "no override":
 * the texture bakes on its param defaults. Tight-packed (no std140 padding),
 * since these feed a CPU texture generator, not a GPU uniform block.
 */
#define WORLD_TEXTURE_PARAM_CAP 64

struct world {
	uint32_t         count;       /* high-water mark, not the live count */
	uint32_t         name_bytes;  /* bytes used in names */
	/*
	 * The editor selection's primary member: -1 when nothing is selected,
	 * else the live id everything that acts on one entity acts on — the
	 * gizmo, the inspector, Frame Selection. It is always a member of
	 * `selected_set` below, and it is the most recently added one.
	 */
	int32_t          selected;
	int32_t          outline;     /* game outline: -1 none, else live id */
	uint8_t          alive[WORLD_MAX_ENTITIES];        /* 0 = tombstoned */
	/*
	 * The rest of the selection (#950). A flag column rather than a list of
	 * ids: membership is the question every reader actually asks — the
	 * outline pass asks it once per drawable entity per frame — and a column
	 * answers it in O(1), snapshots with the used prefix like every other
	 * column, and cannot hold a duplicate.
	 */
	uint8_t          selected_set[WORLD_MAX_ENTITIES];
	/* enum world_entity_flag OR-set. Runtime only — see the note above. */
	uint8_t          flags[WORLD_MAX_ENTITIES];
	uint32_t         mask[WORLD_MAX_ENTITIES];         /* component_bit OR-set */
	int32_t          parent[WORLD_MAX_ENTITIES];       /* -1 = root; any live id */
	struct transform local[WORLD_MAX_ENTITIES];        /* authored, relative */
	struct transform world_xform[WORLD_MAX_ENTITIES];  /* derived each tick */
	uint32_t         name_off[WORLD_MAX_ENTITIES];     /* into names; NO_NAME=none */
	uint32_t         render_ref[WORLD_MAX_ENTITIES];   /* valid iff COMPONENT_RENDER */
	uint32_t         material_ref[WORLD_MAX_ENTITIES]; /* valid iff COMPONENT_MATERIAL */
	uint32_t         script_ref[WORLD_MAX_ENTITIES];   /* valid iff COMPONENT_SCRIPT */
	uint32_t         script_param_len[WORLD_MAX_ENTITIES]; /* 0 = no override */
	uint8_t          script_params[WORLD_MAX_ENTITIES][WORLD_SCRIPT_PARAM_CAP];
	uint32_t         material_param_len[WORLD_MAX_ENTITIES]; /* 0 = no override */
	uint8_t          material_params[WORLD_MAX_ENTITIES][WORLD_MATERIAL_PARAM_CAP];
	uint32_t         mesh_param_len[WORLD_MAX_ENTITIES]; /* 0 = no override */
	uint8_t          mesh_params[WORLD_MAX_ENTITIES][WORLD_MESH_PARAM_CAP];
	uint32_t         texture_param_len[WORLD_MAX_ENTITIES]; /* 0 = no override */
	uint8_t          texture_params[WORLD_MAX_ENTITIES][WORLD_TEXTURE_PARAM_CAP];
	char             names[WORLD_NAME_BYTES];          /* NUL-terminated blob */
};

/* Clear the world to zero entities. Column backing storage is left as-is. */
void world_reset(struct world *w);

/*
 * Append a new entity and return its id, or -1 if the world is full or parent
 * is neither -1 nor a live existing entity. The new id is always above every
 * id handed out before it; that is a property of appending, not a hierarchy
 * invariant, and world_set_parent may put the entity under a higher id later.
 */
int32_t world_create_entity(struct world *w, int32_t parent,
			    const struct transform *local, uint32_t mask);

/*
 * Whether `a` is an ancestor of `e` — the question a reparent has to answer
 * before it can accept one. False when either id is out of range, when they
 * are equal, and when the walk from e runs deeper than the world can nest
 * (which a cycle in a corrupt column is the only way to do).
 */
int32_t world_is_ancestor(const struct world *w, int32_t a, int32_t e);

/*
 * Move a live entity under `parent` (-1 = root), keeping its **local**
 * transform, and recompose its subtree's world transforms off the new
 * ancestry. Returns 0, or -1 without touching anything when e is not live,
 * when parent is neither -1 nor live, when parent is e itself, or when parent
 * is a descendant of e — the drop that would make a cycle. A caller reports
 * that refusal; it is not a silent no-op, because a drag that appears to do
 * nothing is indistinguishable from one the editor dropped on the floor.
 *
 * Keeping the local transform rather than the world one is the deliberate
 * half: `local` is what the scene file holds and what the inspector shows, so
 * a reparent stays a pure hierarchy edit that inverts exactly. Preserving the
 * world pose would mean dividing by the new parent's scale, which is undefined
 * for the zero-scale entities scenes legitimately contain.
 */
int32_t world_set_parent(struct world *w, int32_t e, int32_t parent);

/*
 * Deep-copy entity e and every live descendant, appending the copies under e's
 * own parent, and return the new subtree root's id (-1 if e is not live, if the
 * copies would not fit WORLD_MAX_ENTITIES, or if their names would not fit the
 * blob). Nothing is created unless all of it fits: a half-copied subtree is
 * worse than a refused one.
 *
 * Every column is copied, overrides included, so the copy draws identically the
 * frame it appears. The selection is not: duplicating does not select, because
 * what the copy should be is the caller's policy and the editor has one.
 */
int32_t world_duplicate_entity(struct world *w, int32_t e);

/*
 * Tombstone entity e and, transitively, every entity parented under it. Slots
 * are never reused, so no surviving entity's id (or stored parent ref) moves.
 * Out-of-range ids are ignored. If the selection lands on a tombstoned slot as
 * a result, it is cleared to -1.
 */
void world_destroy_entity(struct world *w, int32_t e);

/*
 * Overwrite a live entity's local transform; out-of-range or tombstoned ids
 * are ignored. world_xform for e and its live descendants is recomposed
 * immediately (off each ancestor's current world_xform); every other entity
 * is left exactly as it was, so an edit outside the tick never disturbs a
 * script-authored pose elsewhere in the world (see world_snapshot_restore).
 */
void world_set_transform(struct world *w, int32_t e,
			 const struct transform *local);

/*
 * Name a live entity (setting COMPONENT_NAME). The name is appended to the
 * blob — any previous name's bytes are abandoned, not reclaimed. A NULL or
 * empty name clears COMPONENT_NAME instead. Returns 0 on success, or -1 if the
 * id is out of range / tombstoned or the name would overflow the blob.
 */
int32_t world_set_name(struct world *w, int32_t e, const char *name);

/*
 * Bind a live entity to a mesh: store render_ref and set COMPONENT_RENDER. A
 * zero render_ref unbinds instead, clearing COMPONENT_RENDER (so "no mesh" is a
 * cleared component, not a dangling ref). Out-of-range or tombstoned ids are
 * ignored.
 */
void world_set_render_ref(struct world *w, int32_t e, uint32_t render_ref);

/*
 * Bind a live entity to a material: store material_ref and set
 * COMPONENT_MATERIAL. A zero material_ref unbinds instead, clearing
 * COMPONENT_MATERIAL (mirrors world_set_render_ref). Out-of-range or
 * tombstoned ids are ignored.
 */
void world_set_material_ref(struct world *w, int32_t e, uint32_t material_ref);

/*
 * Bind a live entity to a behavior script: store script_ref (an asset id) and
 * set COMPONENT_SCRIPT. A zero script_ref unbinds instead, clearing
 * COMPONENT_SCRIPT (mirrors world_set_render_ref). Out-of-range or tombstoned
 * ids are ignored.
 */
void world_set_script_ref(struct world *w, int32_t e, uint32_t script_ref);

/*
 * Override entity e's script parameters with `len` tight-packed bytes (the
 * layout its bound script's params clause reports). A len of 0 clears the
 * override, so the script falls back to its param defaults. Bytes beyond
 * WORLD_SCRIPT_PARAM_CAP are dropped. Out-of-range or tombstoned ids are
 * ignored. Independent of COMPONENT_SCRIPT: an override can be authored before
 * (or after) a script is bound.
 */
void world_set_script_params(struct world *w, int32_t e,
			     const uint8_t *bytes, uint32_t len);

/*
 * Entity e's script-parameter override bytes, or NULL when it has none. *len
 * (may be NULL) receives the byte count. The pointer is into the world and is
 * valid until the next mutation of e's override.
 */
const uint8_t *world_script_params(const struct world *w, uint32_t e,
				   uint32_t *len);

/*
 * Override entity e's material parameters with `len` std140 Material-block bytes
 * (the layout after a material's shader-ref header — what the renderer uploads
 * to the Material UBO). A len of 0 clears the override, so the entity falls back
 * to its bound material asset's own params. Bytes beyond WORLD_MATERIAL_PARAM_CAP
 * are dropped. Out-of-range or tombstoned ids are ignored. Independent of
 * COMPONENT_MATERIAL: an override can be authored before (or after) a material
 * is bound, and it only replaces the params — the shader/pipeline still comes
 * from the bound material asset.
 */
void world_set_material_params(struct world *w, int32_t e,
			       const uint8_t *bytes, uint32_t len);

/*
 * Entity e's material-parameter override bytes, or NULL when it has none. *len
 * (may be NULL) receives the byte count. The pointer is into the world and is
 * valid until the next mutation of e's override.
 */
const uint8_t *world_material_params(const struct world *w, uint32_t e,
				     uint32_t *len);

/*
 * Override entity e's mesh parameters with `len` tight-packed bytes (the layout
 * its bound mesh's params clause reports). A len of 0 clears the override, so
 * the mesh falls back to its param defaults. Bytes beyond WORLD_MESH_PARAM_CAP
 * are dropped. Out-of-range or tombstoned ids are ignored. Independent of
 * COMPONENT_RENDER: an override can be authored before (or after) a mesh is
 * bound, and it only reshapes the geometry — the mesh asset still comes from the
 * bound render_ref.
 */
void world_set_mesh_params(struct world *w, int32_t e,
			   const uint8_t *bytes, uint32_t len);

/*
 * Entity e's mesh-parameter override bytes, or NULL when it has none. *len (may
 * be NULL) receives the byte count. The pointer is into the world and is valid
 * until the next mutation of e's override.
 */
const uint8_t *world_mesh_params(const struct world *w, uint32_t e,
				 uint32_t *len);

/*
 * Override entity e's texture parameters with `len` tight-packed bytes (the
 * layout its material's bound texture's params clause reports). A len of 0 clears
 * the override, so the texture bakes on its param defaults. Bytes beyond
 * WORLD_TEXTURE_PARAM_CAP are dropped. Out-of-range or tombstoned ids are
 * ignored. Independent of COMPONENT_MATERIAL: an override can be authored before
 * (or after) a material is bound, and it only re-bakes the texture — which
 * texture and at what resolution still come from the material's texture slot.
 */
void world_set_texture_params(struct world *w, int32_t e,
			      const uint8_t *bytes, uint32_t len);

/*
 * Entity e's texture-parameter override bytes, or NULL when it has none. *len
 * (may be NULL) receives the byte count. The pointer is into the world and is
 * valid until the next mutation of e's override.
 */
const uint8_t *world_texture_params(const struct world *w, uint32_t e,
				    uint32_t *len);

/*
 * Editor selection model — a set, with one member singled out as primary.
 *
 * ## Why a set, and why the primary survives it
 *
 * The selection was one id until #950 needed the outliner to select several
 * rows at once. It could not answer that editor-side: the viewport, the gizmo
 * and the inspector all read the selection from here, and a second copy living
 * in the editor is exactly the "two views that disagree" failure #947 exists to
 * prevent. So the set lives where the single id lived.
 *
 * `selected` stays, and stays meaning what it meant: the one entity a tool that
 * acts on one entity acts on. Every existing reader — the gizmo, the outline
 * pass, Frame Selection, a game reading the picked piece — keeps working
 * against it unchanged, and only the readers that want the whole set ask for
 * it. The primary is the most recently added member, which is what makes
 * ctrl-clicking a fourth row put the gizmo on that row.
 *
 * set accepts -1 (none) or a live entity id; any other value (out of range or
 * tombstoned) is ignored, leaving the selection unchanged. It **replaces** the
 * set, which is what an unmodified click means.
 */
void    world_set_selected(struct world *w, int32_t e);
int32_t world_get_selected(const struct world *w);

/*
 * Add a live entity to the selection and make it primary. A no-op for an id
 * that is not live, and for one already in the set it still moves the primary —
 * ctrl-clicking a row that is already selected is how a reader says "act on
 * this one".
 */
void    world_select_add(struct world *w, int32_t e);

/*
 * Drop an entity from the selection. When it was the primary, the lowest
 * remaining member becomes primary (-1 when the set is now empty). Lowest
 * rather than "the one added before it" because the world keeps no add order,
 * and inventing one to hold a stack would be state that only this case reads.
 */
void    world_select_remove(struct world *w, int32_t e);

/* Empty the selection. The primary goes to -1. */
void    world_select_clear(struct world *w);

/*
 * The editor flags — hidden and locked. Setting them on a dead or out-of-range
 * id is ignored; reading one answers 0. Neither is undoable and neither is
 * saved; both are cleared when an entity is created and when a scene is
 * ingested, so opening a project starts with everything visible and unlocked.
 */
void    world_set_entity_flags(struct world *w, int32_t e, uint32_t flags);
uint32_t world_entity_flags(const struct world *w, int32_t e);

/* Whether e is in the selection. False for out-of-range and tombstoned ids. */
int32_t world_is_selected(const struct world *w, int32_t e);

/*
 * The selection's members in ascending id order, written into `out` (up to
 * `cap` of them), returning how many there are — which may exceed `cap`, so a
 * caller that cares can tell a full buffer from a complete answer.
 */
uint32_t world_selection(const struct world *w, int32_t *out, uint32_t cap);

/*
 * The game-driven outline target: the entity the renderer's selection-outline
 * pass highlights in-game, independent of the editor `selected` above. A game's
 * rules set it (a chess piece the player picked up) so the outline shows outside
 * editor chrome; -1 = none. Same stale-id discipline as selected.
 */
void    world_set_outline(struct world *w, int32_t e);
int32_t world_get_outline(const struct world *w);

/* The entity's name, or NULL when it has no COMPONENT_NAME / no stored name. */
const char *world_entity_name(const struct world *w, uint32_t e);

/*
 * Ingest a decoded scene into the world's columns, replacing all current
 * entities. Returns 0 on success, -1 if the scene exceeds WORLD_MAX_ENTITIES
 * or its names overflow WORLD_NAME_BYTES. The scene is only read; the caller
 * still owns and must free it.
 */
int32_t world_ingest_scene(struct world *w, const struct scene *s);

/*
 * Editor undo snapshots. A snapshot is a full-fidelity copy of the world's
 * used prefix — every persistent column (including world_xform, captured
 * verbatim rather than re-derived), the name blob, and the selection — sized
 * to the live high-water mark rather than the fixed 4096 cap, so it stays
 * proportional to scene size. capture returns NULL on allocation failure;
 * restore and free tolerate a NULL snapshot. Restoring reinstates the exact
 * entity ids and tombstones (so a destroyed subtree comes back whole) and the
 * exact world_xform each entity carried at capture time — a checkpoint jump,
 * not a tick, so a script-authored pose elsewhere in the world is reinstated
 * rather than reset to its rest pose.
 *
 * All snapshot storage comes from the injected memory_api (never libc malloc);
 * free must be handed the same allocator that captured the snapshot.
 */
struct memory_api;
struct world_snapshot;
struct world_snapshot *world_snapshot_capture(const struct world *w,
					      const struct memory_api *mem);
void world_snapshot_restore(struct world *w, const struct world_snapshot *s);
void world_snapshot_free(struct world_snapshot *s,
			 const struct memory_api *mem);

/*
 * Export the world's live entities as an at-rest struct scene — the inverse of
 * world_ingest_scene, and the serialization side of scene_encode.  Tombstoned
 * slots are dropped and parent indices remapped onto the compacted ordering.
 *
 * **The output is topologically ordered even though the world is not** (#950):
 * indices are assigned parent-before-child rather than by id, because struct
 * scene's ordering (scene.h) is a file-format guarantee that a decoder on the
 * other side of a version boundary reads in one pass.  Siblings keep ascending
 * id order within that, so the same world exports byte-identically every time.
 *
 * Names are re-packed into a gap-free blob.  The editor selection is
 * session-local and is deliberately not exported.
 *
 * Allocates through mem; on success the caller owns the result and frees
 * entities, names, then the struct — matching scene_decode's contract.  An
 * all-tombstoned (or empty) world yields a valid zero-entity scene.  Returns
 * NULL on a NULL mem or allocation failure.
 */
struct scene *world_export_scene(const struct world *w,
				 const struct memory_api *mem);

/*
 * Resolve world_xform for every live entity: roots (parent -1) copy their local
 * transform, children compose parent-world * local. dt is unused but kept to
 * match the system signature.
 *
 * Each entity is resolved after its ancestors rather than in index order, since
 * #950 stopped the columns being topological — see entity.c. The walk is still
 * linear in the live entity count: an ancestor chain is climbed only until it
 * reaches one already resolved, and every entity is resolved exactly once.
 */
void world_propagate_transforms(struct world *w, float dt);

/* Run the per-frame system list (currently just transform propagation). */
void world_tick(struct world *w, float dt);

#endif /* WORLD_H */
