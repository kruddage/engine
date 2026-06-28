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
};

#endif /* ASSET_API_H */
