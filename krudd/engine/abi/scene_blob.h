/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SCENE_BLOB_H
#define SCENE_BLOB_H

#include "scene.h"

#include <stdint.h>

/*
 * On-the-wire scene blob — the envelope struct scene.h calls for but leaves
 * to whoever exports/imports a scene: a header stamped with a format version,
 * followed by entity_count scene_entity records (memcpy'd verbatim —
 * scene_entity is already the packed, little-endian transfer layout, see
 * scene.h) and then string_bytes of name blob. Mirrors mesh_blob.h's
 * header-then-arrays shape, including its "sentinel, not the engine's
 * semver" posture: format_version bumps only when this layout itself
 * changes, independent of and decoupled from ENGINE_VERSION_*, so entity.c
 * (compiled into several small native test binaries, not just the plugin)
 * never needs the generated version.h include path.
 */
#define SCENE_BLOB_MAGIC 0x454e4353u /* "SCNE" */
#define SCENE_BLOB_FORMAT_VERSION 1u

struct scene_blob_header {
	uint32_t magic;          /* SCENE_BLOB_MAGIC */
	uint32_t format_version; /* SCENE_BLOB_FORMAT_VERSION */
	uint32_t entity_count;
	uint32_t string_bytes;   /* size of trailing name blob */
};

struct memory_api;

/* Total byte size of the encoded blob for scene s. */
uint32_t scene_blob_size(const struct scene *s);

/*
 * Pack s into out (which must be at least scene_blob_size(s) bytes).
 * Returns the number of bytes written, or 0 if cap is too small.
 */
uint32_t scene_blob_encode(const struct scene *s, void *out, uint32_t cap);

/*
 * Unpack a blob previously produced by scene_blob_encode. Returns NULL on a
 * bad magic, an unsupported format_version, a truncated buffer, or an
 * allocation failure. On success the caller owns the result and frees it the
 * same way scene_blob_decode's own decoder contract in scene.h describes:
 * entities, names, then the struct itself, all through mem.
 */
struct scene *scene_blob_decode(const void *bytes, uint32_t size,
				const struct memory_api *mem);

#endif /* SCENE_BLOB_H */
