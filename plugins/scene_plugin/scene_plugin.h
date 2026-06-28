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

#endif /* SCENE_PLUGIN_H */
