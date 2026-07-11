/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ASSET_API_H
#define ASSET_API_H

#include <stdint.h>

/*
 * Asset kind discriminator.  ASSET_KIND_NORMAL covers all assets that
 * enter the catalog via asset_request().  ASSET_KIND_PRIMITIVE marks
 * engine-provided, read-only built-ins seeded at startup.
 */
#define ASSET_KIND_NORMAL    0
#define ASSET_KIND_PRIMITIVE 1

/*
 * Asset origin discriminator.  ASSET_ORIGIN_FETCHED is the default for
 * assets loaded via asset_request().  ASSET_ORIGIN_AUTHORED marks assets
 * created from editor-supplied bytes (no fetch); only authored assets are
 * persisted by the future persistence layer.
 */
#define ASSET_ORIGIN_FETCHED  0
#define ASSET_ORIGIN_AUTHORED 1

/*
 * Asset type discriminator.  Identifies what the asset represents,
 * independent of how it entered the catalog (kind).
 */
#define ASSET_TYPE_UNKNOWN  0
#define ASSET_TYPE_MESH     1
#define ASSET_TYPE_TEXTURE  2
#define ASSET_TYPE_MATERIAL 3
#define ASSET_TYPE_SHADER   4
#define ASSET_TYPE_FONT     5
#define ASSET_TYPE_SCENE    6
#define ASSET_TYPE_TEXT     7
#define ASSET_TYPE_SCRIPT   8

/*
 * Per-entry snapshot filled by asset_api.info().  path points into the
 * catalog's live storage; valid until the next eviction.  Copy the
 * string if you need to hold it past the current frame.
 */
struct asset_info {
	const char *path;
	int32_t     state;     /* asset_state */
	uint32_t    size;      /* bytes; 0 until loaded */
	int32_t     refs;
	int32_t     kind;      /* ASSET_KIND_* */
	int32_t     read_only; /* 1 = built-in, never evicted */
	int32_t     type;      /* ASSET_TYPE_* */
	uint32_t    id;        /* stable catalog id; 0 = none */
	int32_t     origin;    /* ASSET_ORIGIN_* */
};

/*
 * Key/value pair returned by asset_api.describe().  Both strings point
 * into static storage and are valid for the process lifetime.
 */
struct asset_decl_field {
	const char *key;
	const char *value;
};

/*
 * Read-only catalog enumeration.  Obtain via
 * subsystem_manager_get_api(mgr, "asset").
 */
struct asset_api {
	/* Number of entries currently in the catalog. */
	uint32_t (*count)(void);
	/*
	 * Snapshot entry i into *out.
	 * Returns 0 on success, -1 if i is out of range or out is NULL.
	 */
	int32_t  (*info)(uint32_t i, struct asset_info *out);
	/*
	 * Fill *out (caller-allocated array of max fields) with the
	 * asset's declaration.  Returns the number of fields written, or
	 * 0 if i is out of range.  Built-in decl strings are valid for the
	 * process lifetime; authored decl strings (set via asset_mut.set_decl)
	 * are valid until the entry is evicted or its declaration is next set.
	 */
	uint32_t (*describe)(uint32_t i, struct asset_decl_field *out,
			     uint32_t max);
	/*
	 * Resolve a stable id to its current catalog entry.
	 * Returns 0 and fills *out on hit; returns -1 if id is unknown
	 * or out is NULL.  id 0 always returns -1 (reserved for "none").
	 */
	int32_t  (*find)(uint32_t id, struct asset_info *out);
	/*
	 * Borrow an asset's loaded bytes by id.  Returns NULL if id is
	 * unknown or the asset is not in ASSET_LOADED state.  *out_size
	 * may be NULL.  The pointer is valid until the entry is evicted
	 * or mutated.
	 */
	const void *(*get_data)(uint32_t id, uint32_t *out_size);
};

/*
 * Mutation API for authored project assets.  Obtain via
 * subsystem_manager_get_api(mgr, "asset_mut").
 *
 * Authored assets are born-loaded (no fetch) and are the only origin
 * that the future persistence layer will enumerate and save.  The
 * create() function is intentionally generic so that other callers
 * (e.g. file-drop import, #179) can inject bytes with any path/type
 * without going through emscripten_fetch.
 */
struct asset_mut_api {
	/*
	 * Create a born-loaded authored project asset from bytes.
	 * Copies the bytes.  Returns the new stable id, or 0 on failure
	 * (cache full, duplicate path, or bad args).
	 */
	uint32_t (*create)(const char *path, int32_t type,
			   const void *bytes, uint32_t size);
	/*
	 * Replace an authored asset's bytes in place (by id).
	 * Returns 0 on success, -1 on miss / not-authored / OOM.
	 */
	int32_t  (*set_data)(uint32_t id, const void *bytes,
			     uint32_t size);
	/*
	 * Delete an authored asset by id.
	 * Returns 0 on success, -1 on miss / not-authored.
	 */
	int32_t  (*destroy)(uint32_t id);
	/*
	 * Replace an authored asset's declaration metadata (by id) with n
	 * key/value pairs (e.g. a shader's stage/dialect).  Copies the
	 * strings; they then surface through asset_api.describe().  n == 0
	 * clears the declaration.  Returns 0 on success, -1 on miss /
	 * not-authored / n too large / null fields.
	 */
	int32_t  (*set_decl)(uint32_t id,
			     const struct asset_decl_field *fields,
			     uint32_t n);
	/*
	 * Inject a born-loaded authored asset under a caller-supplied stable
	 * id — the id-preserving counterpart to create(), used to rehydrate a
	 * persisted asset so its identity survives a reload (the persistence
	 * key stays valid across sessions).  Copies the bytes and advances the
	 * id allocator past `id` so a later create() never reuses it.  Returns
	 * 0 on success, -1 on failure (id 0, cache full, or a duplicate id or
	 * path).  Intended for the startup rehydration path, before the user
	 * authors new assets.  ABI-additive: appended after the original four.
	 */
	int32_t  (*inject)(uint32_t id, const char *path, int32_t type,
			   const void *bytes, uint32_t size);
};

#endif /* ASSET_API_H */
