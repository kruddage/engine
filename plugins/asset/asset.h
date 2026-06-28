/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ASSET_H
#define ASSET_H

#include "asset_api.h"
#include <stdint.h>

#define ASSET_PATH_MAX 256

typedef enum {
	ASSET_PENDING = 0,
	ASSET_LOADED,
	ASSET_ERROR,
} asset_state;

/* Increment the ref count and begin loading if not already cached. */
void asset_request(const char *path);

/* Return the current load state. Returns ASSET_ERROR if path is unknown. */
asset_state asset_state_of(const char *path);

/*
 * Return a pointer to loaded data, or NULL if not yet loaded.
 * out_size may be NULL. The pointer is valid until asset_release
 * drops the ref count to zero.
 */
const void *asset_get(const char *path, uint32_t *out_size);

/*
 * Decrement the ref count. Frees data when count reaches zero and the
 * load is complete; in-flight WASM fetches are not cancelled.
 */
void asset_release(const char *path);

/* Register a decoder for a file extension (without leading dot). */
void  asset_codec_register(const char *ext,
			   void *(*decode)(const void *bytes, uint32_t size));

/*
 * Extracts the extension from path, retrieves raw bytes via asset_get,
 * invokes the registered decoder, and returns the result.
 * Returns NULL if no decoder is registered or the asset is not loaded.
 * Caller owns the returned pointer.
 */
void *asset_codec_get_typed(const char *path);

/*
 * Initialize the asset subsystem (seeds built-in primitives).
 * Called automatically by the plugin lifecycle; also exposed so
 * unit tests can prime the catalog before querying it.
 */
void asset_init(void);

/* Read-only catalog enumeration (also published as the "asset" subsystem api). */
uint32_t asset_catalog_count(void);
int32_t  asset_catalog_info(uint32_t i, struct asset_info *out);

#endif /* ASSET_H */
