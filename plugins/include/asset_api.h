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
	 * 0 if i is out of range.  Strings point into static storage.
	 */
	uint32_t (*describe)(uint32_t i, struct asset_decl_field *out,
			     uint32_t max);
	/*
	 * Resolve a stable id to its current catalog entry.
	 * Returns 0 and fills *out on hit; returns -1 if id is unknown
	 * or out is NULL.  id 0 always returns -1 (reserved for "none").
	 */
	int32_t  (*find)(uint32_t id, struct asset_info *out);
};

#endif /* ASSET_API_H */
