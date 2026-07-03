/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SCENE_PLUGIN_H
#define SCENE_PLUGIN_H

#include <stdint.h>

/*
 * Decode a raw .scene byte buffer into an allocated struct scene *.
 * Returns NULL on format error, version mismatch, topological-order
 * violation, or allocation failure.  On success the caller owns the
 * result and must free entities, names, and the struct individually.
 */
void *scene_decode(const void *bytes, uint32_t size);

struct scene;

/*
 * Encode a struct scene into a canonical .scene v1 byte buffer — the exact
 * inverse of scene_decode.  Deterministic: the same scene always produces
 * byte-identical output, which is what lets scene content be content-addressed.
 * The trailing name-blob size is derived from the entities' name offsets, so
 * the encoding of decode(bytes) reproduces bytes exactly.
 *
 * The buffer is allocated via the module memory_api; the caller owns it and
 * frees it with a single free().  *out_size (may be NULL) receives the byte
 * length.  Returns NULL on allocation failure.
 */
void *scene_encode(const struct scene *s, uint32_t *out_size);

#endif /* SCENE_PLUGIN_H */
