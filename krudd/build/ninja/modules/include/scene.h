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
};

#define SCENE_NO_NAME 0xFFFFFFFFu

/*
 * On-disk .scene v3 — packed, little-endian.  v3 appends script_ref to each
 * entity record (v2 had render_ref/material_ref as the last two fields).  The
 * byte codec that reads/writes this header is not implemented yet; version is
 * stamped here so the codec, when written, can distinguish a v2 record (no
 * script_ref) from a v3 one.
 */
struct scene_header {
	uint8_t  magic[4];     /* "KSCN" */
	uint32_t version;      /* 3 */
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
	uint32_t script_ref;   /* valid iff COMPONENT_SCRIPT set (v3+) */
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
