/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "branch_serialize.h"

#include "cas.h"

#include <stddef.h>

/*
 * GATE STUB (#215 live-save serialize).  Track B replaces this body with the
 * real capture: gather the world bytes (via the "scene" entity_api
 * export_scene_bytes) and the authored catalog (via the "asset" asset_api),
 * write blobs to `store`, and assemble a manifest per branch_manifest.h.
 * Until then it reports "nothing captured" so the host and native build stay
 * green.
 */
int32_t branch_serialize_capture(struct cas *store,
				 struct subsystem_manager *mgr,
				 cas_hash_t *out)
{
	(void)store;
	(void)mgr;
	(void)out;
	return -1;
}
