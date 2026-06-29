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
 * Set a live entity's render_ref and COMPONENT_RENDER bit; out-of-range or
 * tombstoned ids are ignored.
 */
void world_set_render_ref(struct world *w, int32_t e, uint32_t render_ref);

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
 * Resolve world_xform for every live entity in one forward pass, relying on
 * the parent-before-child ordering. Roots (parent -1) copy their local
 * transform; children compose parent-world * local. dt is unused but kept to
 * match the system signature.
 */
void world_propagate_transforms(struct world *w, float dt);

/* Run the per-frame system list (currently just transform propagation). */
void world_tick(struct world *w, float dt);

#endif /* WORLD_H */
