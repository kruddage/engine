/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BRANCH_INGEST_H
#define BRANCH_INGEST_H

#include "cas.h"

#include <stdint.h>

struct subsystem_manager;

/*
 * Ingest side of the switch mechanism (#215) and restore (#216): resolve a
 * manifest hash into a working set and hand it to the engine as one atomic
 * reload — the world swaps via the "scene" entity_api (ingest_scene_bytes →
 * world_ingest_scene) and the catalog swaps with it via the "asset_mut" api
 * (inject / set_data for entries in the manifest, destroy for authored entries
 * no longer present).  This is the inverse of branch_serialize_capture and
 * reads the same branch_manifest.h convention.
 *
 * The swap should be as close to atomic as the apis allow: decode the whole
 * manifest first, then apply, so a decode failure leaves the live state
 * untouched.  Returns 0 on success; -1 on a missing api, an absent/invalid
 * manifest, or an ingest failure.
 *
 * Implementation note for the track: keep a pure, native-testable core that
 * decodes a manifest hash from `store` into { scene bytes, list of asset
 * entries }, with this glue applying that to the live subsystems.
 */
int32_t branch_ingest_apply(struct cas *store,
			    struct subsystem_manager *mgr,
			    cas_hash_t manifest);

#endif /* BRANCH_INGEST_H */
