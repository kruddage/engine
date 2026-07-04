/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "branch_ingest.h"

#include "cas.h"

#include <stddef.h>

/*
 * GATE STUB (#215 switch / #216 restore ingest).  Track C replaces this body
 * with the real apply: decode the manifest from `store`, swap the world (via
 * the "scene" entity_api ingest_scene_bytes) and the catalog (via the
 * "asset_mut" api) in one atomic reload, per branch_manifest.h.  Until then it
 * reports failure so the host and native build stay green.
 */
int32_t branch_ingest_apply(struct cas *store,
			    struct subsystem_manager *mgr,
			    cas_hash_t manifest)
{
	(void)store;
	(void)mgr;
	(void)manifest;
	return -1;
}
