/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SCENE_H
#define SCENE_H

#include "math_types.h"

#include <stdint.h>

enum component_bit {
	COMPONENT_NAME     = 1u << 0,
	COMPONENT_RENDER   = 1u << 1,
	COMPONENT_MATERIAL = 1u << 2,
	COMPONENT_SCRIPT   = 1u << 3,
	/*
	 * A directional light. The entity contributes a light to the scene; the
	 * renderer takes its world-space direction from the entity's transform
	 * rotation (so moving/rotating the entity steers the light) and needs no
	 * per-entity light data column yet — colour/intensity are engine
	 * constants for now, a documented next increment. Carried in the mask
	 * like every other component, so snapshot/undo and scene ingest/export
	 * round-trip it for free.
	 */
	COMPONENT_LIGHT    = 1u << 4,
};

#define SCENE_NO_NAME 0xFFFFFFFFu

/*
 * Packed, little-endian transfer layout — the flat struct scene_decode
 * produces and scene_encode consumes.  Not itself an on-disk envelope: it
 * carries no magic or format version, because this struct isn't the export
 * format.  Whatever wraps it for save files / asset export stamps its own
 * envelope with the engine's semver (ENGINE_VERSION_STRING, see
 * core/version.h), so compatibility is decided at that layer, not here.
 */
struct scene_header {
	uint32_t entity_count;
	uint32_t string_bytes; /* size of trailing name blob */
};

/* entity_count records, topological order (parent < own index). */
struct scene_entity {
	uint32_t mask;         /* enum component_bit */
	int32_t  parent;       /* -1 = root */
	vec3_t   position;
	quat_t   rotation;
	vec3_t   scale;
	uint32_t name_off;     /* byte offset into name blob; SCENE_NO_NAME = none */
	uint32_t render_ref;   /* valid iff COMPONENT_RENDER set */
	uint32_t material_ref; /* valid iff COMPONENT_MATERIAL set */
	uint32_t script_ref;   /* valid iff COMPONENT_SCRIPT set */
};

/*
 * Decoder output.  Caller owns all three allocations and must free them
 * individually: entities, names, then the struct itself.
 */
struct scene {
	uint32_t             count;
	struct scene_entity *entities;
	char                *names;   /* NUL-terminated name blob; NULL if none */
};

#endif /* SCENE_H */
