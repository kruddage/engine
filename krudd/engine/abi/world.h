/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WORLD_H
#define WORLD_H

#include "scene.h"

#include <stdint.h>

/*
 * Runtime entity world — flat struct-of-arrays keyed by a dense entity id.
 * Distinct from the at-rest `struct scene` the .scene decoder returns: the
 * decoder produces a flat transfer struct; the entity system ingests it into
 * these parallel columns (see world_ingest_scene).
 *
 * Hierarchy is the int32_t parent[] column (-1 = root), never pointers.
 * Entities are stored in topological order (parent index < child index),
 * which lets world_propagate_transforms resolve every world transform in a
 * single forward pass. Creation appends, preserving that order; destruction
 * tombstones a slot (and its descendants) without shifting any index, so the
 * parent refs stored in surviving entities stay valid — a naive swap-remove
 * would shift indices and silently corrupt the hierarchy.
 *
 * enum component_bit, struct scene, struct scene_entity and SCENE_NO_NAME
 * come from scene.h; the runtime mask mirrors the file mask exactly so ingest
 * stays a near-memcpy. A transform is implicit on every entity (no bit).
 */

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

struct transform {
	float position[3];
	float rotation[4];   /* quaternion, xyzw */
	float scale[3];
};

struct world {
	uint32_t         count;       /* high-water mark, not the live count */
	uint32_t         name_bytes;  /* bytes used in names */
	int32_t          selected;    /* editor selection: -1 none, else live id */
	uint8_t          alive[WORLD_MAX_ENTITIES];        /* 0 = tombstoned */
	uint32_t         mask[WORLD_MAX_ENTITIES];         /* component_bit OR-set */
	int32_t          parent[WORLD_MAX_ENTITIES];       /* -1 root; parent<child */
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
	char             names[WORLD_NAME_BYTES];          /* NUL-terminated blob */
};

/* Clear the world to zero entities. Column backing storage is left as-is. */
void world_reset(struct world *w);

/*
 * Append a new entity and return its id, or -1 if the world is full or parent
 * is neither -1 nor a live existing entity. Appending keeps the parent-before-
 * child ordering: parent (when >= 0) is always less than the new id.
 */
int32_t world_create_entity(struct world *w, int32_t parent,
			    const struct transform *local, uint32_t mask);

/*
 * Tombstone entity e and, transitively, every entity parented under it. Slots
 * are never reused, so no surviving entity's id (or stored parent ref) moves.
 * Out-of-range ids are ignored. If the selection lands on a tombstoned slot as
 * a result, it is cleared to -1.
 */
void world_destroy_entity(struct world *w, int32_t e);

/*
 * Overwrite a live entity's local transform; out-of-range or tombstoned ids
 * are ignored. The change reaches world_xform after the next propagate.
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
 * Editor selection model. set accepts -1 (none) or a live entity id; any
 * other value (out of range or tombstoned) is ignored, leaving the selection
 * unchanged. get returns the current selection (-1 when none).
 */
void    world_set_selected(struct world *w, int32_t e);
int32_t world_get_selected(const struct world *w);

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
 * used prefix — every persistent column, the name blob, and the selection —
 * sized to the live high-water mark rather than the fixed 4096 cap, so it
 * stays proportional to scene size. capture returns NULL on allocation
 * failure; restore and free tolerate a NULL snapshot. Restoring reinstates the
 * exact entity ids and tombstones (so a destroyed subtree comes back whole)
 * and re-derives world_xform.
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
 * slots are dropped and parent indices remapped onto the compacted ordering
 * (which stays topological: a live entity never has a tombstoned parent, since
 * destroy cascades).  Names are re-packed into a gap-free blob.  The editor
 * selection is session-local and is deliberately not exported.
 *
 * Allocates through mem; on success the caller owns the result and frees
 * entities, names, then the struct — matching scene_decode's contract.  An
 * all-tombstoned (or empty) world yields a valid zero-entity scene.  Returns
 * NULL on a NULL mem or allocation failure.
 */
struct scene *world_export_scene(const struct world *w,
				 const struct memory_api *mem);

/*
 * Resolve world_xform for every live entity in one forward pass, relying on
 * the parent-before-child ordering. Roots (parent -1) copy their local
 * transform; children compose parent-world * local. dt is unused but kept to
 * match the system signature.
 */
void world_propagate_transforms(struct world *w, float dt);

/* Run the per-frame system list (currently just transform propagation). */
void world_tick(struct world *w, float dt);

#endif /* WORLD_H */
