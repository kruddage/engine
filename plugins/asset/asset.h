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
uint32_t    asset_catalog_count(void);
int32_t     asset_catalog_info(uint32_t i, struct asset_info *out);
/*
 * Fill *out (caller array of max fields) with entry i's declaration.
 * Returns the number of fields written, or 0 if i is out of range or has
 * no declaration.  Built-in decl strings live for the process lifetime;
 * authored decl strings (set via asset_mut_set_decl) are valid until the
 * entry is evicted or its declaration is next set.
 */
uint32_t    asset_catalog_describe(uint32_t i, struct asset_decl_field *out,
				   uint32_t max);
/*
 * Resolve a stable id to its current catalog entry.
 * Returns 0 on hit (fills *out), -1 on miss or NULL out.
 * id 0 is reserved for "none" and always returns -1.
 */
int32_t     asset_catalog_find(uint32_t id, struct asset_info *out);
/*
 * Borrow an asset's loaded bytes by id.  Returns NULL if id is
 * unknown or the asset is not loaded.  *out_size may be NULL.
 * Pointer valid until the entry is evicted or mutated.
 */
const void *asset_catalog_get_data(uint32_t id, uint32_t *out_size);

/*
 * Mutation API for authored project assets
 * (also published as the "asset_mut" subsystem api).
 *
 * create: allocate a born-loaded authored entry from caller-supplied
 *         bytes.  Returns the new stable id, or 0 on failure (cache
 *         full, duplicate path, or bad args).
 * set_data: replace an authored asset's bytes in place (by id).
 *           Returns 0 on success, -1 on miss / not-authored / OOM.
 * destroy: delete an authored asset by id.
 *          Returns 0 on success, -1 on miss / not-authored.
 * set_decl: replace an authored asset's declaration with n key/value pairs.
 *           n == 0 clears it.  Returns 0 on success, -1 on miss /
 *           not-authored / n too large / null fields.
 */
uint32_t asset_mut_create(const char *path, int32_t type,
			  const void *bytes, uint32_t size);
int32_t  asset_mut_set_data(uint32_t id, const void *bytes, uint32_t size);
int32_t  asset_mut_destroy(uint32_t id);
int32_t  asset_mut_set_decl(uint32_t id, const struct asset_decl_field *fields,
			    uint32_t n);

#endif /* ASSET_H */
