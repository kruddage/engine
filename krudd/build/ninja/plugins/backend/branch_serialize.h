/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BRANCH_SERIALIZE_H
#define BRANCH_SERIALIZE_H

#include "cas.h"

#include <stdint.h>

struct subsystem_manager;

/*
 * Serialize side of live-save (#215): capture the current whole-project state
 * — the live world plus the authored catalog — into a content-addressed
 * manifest and return its hash.  The host calls this on the debounced dirty
 * flush, then advances the active branch (branches_commit) and captures an
 * auto-snapshot from the returned hash.
 *
 * Reaches the world via the "scene" entity_api (export_scene_bytes → canonical
 * .scene bytes, #235) and the catalog via the "asset" asset_api (get_data over
 * the authored entries).  Content is written to `store` by hash, so unchanged
 * blobs dedup and a capture costs O(changes) (#214).  Entries follow the
 * branch_manifest.h convention.
 *
 * Returns 0 and writes *out on success; -1 on a missing api, an export/encode
 * failure, or OOM (in which case *out is untouched).
 *
 * Implementation note for the track: keep a pure, native-testable core that
 * takes scene bytes + a list of {stable id, asset type, bytes, size} and builds
 * the manifest, with this glue gathering those from the live subsystems.
 */
int32_t branch_serialize_capture(struct cas *store,
				 struct subsystem_manager *mgr,
				 cas_hash_t *out);

#endif /* BRANCH_SERIALIZE_H */
