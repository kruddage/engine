/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ASSET_H
#define ASSET_H

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

#endif /* ASSET_H */
