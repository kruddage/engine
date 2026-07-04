/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BRANCH_MANIFEST_H
#define BRANCH_MANIFEST_H

#include "cas.h"

#include <stdint.h>

/*
 * Shared manifest convention for a whole-project state (#214 manifest shape),
 * used by BOTH the serialize side (branch_serialize.c, #215 live-save) and the
 * ingest side (branch_ingest.c, #215 switch / #216 restore).  Keeping the
 * mapping in one header is what lets those two files be written independently
 * and still agree byte-for-byte on what a manifest means.
 *
 * A manifest is a flat, id-keyed list of struct cas_entry (see cas.h): each
 * entry is { id (stable #187), kind, content hash }.  Two families of entry:
 *
 *   - The SCENE entry: exactly one, at reserved id MANIFEST_ID_SCENE / kind
 *     MANIFEST_KIND_SCENE.  Its blob is the canonical .scene v1 stream (#157)
 *     produced by encoding the live world (world_export_scene + scene_encode,
 *     #235).  A project with an empty world still has this entry (a zero-entity
 *     scene), so a manifest always describes a complete state.
 *
 *   - ASSET entries: one per authored catalog asset (#188), keyed by the
 *     asset's stable id (#187, which survives reload per #236) at kind
 *     MANIFEST_KIND_ASSET.  The blob is the asset's raw bytes (asset_api
 *     get_data on serialize; asset_mut inject/set_data on ingest).  Read-only
 *     seeded assets are NOT captured — only authored/mutable ones, matching
 *     what the persistence layer already saves.
 *
 * Reserved ids never collide with asset stable ids, which start at 1 and only
 * grow (next_asset_id in the asset plugin).  We reserve the top of the id space
 * for engine-owned manifest entries so an asset id can never alias one.
 */

/* Engine-reserved manifest ids live at the top of the u32 space. */
#define MANIFEST_ID_SCENE   0xFFFFFFFFu   /* the one canonical scene blob */

/* Entry kinds.  Asset entries carry the asset's own ASSET_TYPE_* as `kind`
 * is NOT reused here — we tag by role so ingest can dispatch without guessing.
 * The asset's ASSET_TYPE_* is recoverable from its bytes/catalog on ingest. */
#define MANIFEST_KIND_SCENE 1u            /* the reserved scene entry */
#define MANIFEST_KIND_ASSET 2u            /* an authored catalog asset */

/*
 * NOTE on asset type: the flat cas_entry has one `kind` field.  For asset
 * entries we still need the ASSET_TYPE_* to re-create the catalog entry on
 * ingest.  v1 keeps the manifest shape flat by packing the asset type into the
 * high bits of `kind`: kind == MANIFEST_KIND_ASSET | (asset_type << 8).  Use
 * the helpers below rather than open-coding the layout.
 */
#define MANIFEST_KIND_ROLE_MASK  0x000000FFu
#define MANIFEST_KIND_TYPE_SHIFT 8

static inline uint32_t manifest_kind_asset(int32_t asset_type)
{
	return MANIFEST_KIND_ASSET |
	       ((uint32_t)asset_type << MANIFEST_KIND_TYPE_SHIFT);
}

static inline uint32_t manifest_kind_role(uint32_t kind)
{
	return kind & MANIFEST_KIND_ROLE_MASK;
}

static inline int32_t manifest_kind_asset_type(uint32_t kind)
{
	return (int32_t)(kind >> MANIFEST_KIND_TYPE_SHIFT);
}

#endif /* BRANCH_MANIFEST_H */
