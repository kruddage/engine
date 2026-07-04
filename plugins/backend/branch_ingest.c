/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "branch_ingest.h"

#include "branch_manifest.h"
#include "cas.h"
#include "asset_api.h"
#include "entity_api.h"
#include "memory_api.h"
#include "subsystem_manager.h"

#include <stddef.h>
#include <stdio.h>

/*
 * Pure decode core (native-testable without a live world/catalog): resolve a
 * manifest hash into the reserved scene entry plus the flat set of asset
 * entries.  `scratch` must hold at least `max` entries; on success
 * plan->assets aliases scratch, compacted in place to hold only the
 * asset-role entries (the scene entry is pulled out into plan->scene_hash
 * instead of riding alongside).  Returns 0 only if the manifest decodes AND
 * carries the reserved scene entry — branch_manifest.h guarantees a valid
 * manifest always describes a complete state, so a missing scene entry means
 * this hash isn't one.
 */
struct ingest_plan {
	int32_t           has_scene;
	cas_hash_t        scene_hash;
	struct cas_entry *assets;
	uint32_t          asset_count;
};

static int32_t ingest_decode(struct cas *store, cas_hash_t manifest,
			     struct cas_entry *scratch, uint32_t max,
			     struct ingest_plan *plan)
{
	int32_t  n;
	uint32_t i, w;

	if (!store || !scratch || !plan)
		return -1;

	n = cas_get_manifest(store, manifest, scratch, max);
	if (n < 0)
		return -1;

	plan->has_scene  = 0;
	plan->scene_hash = CAS_HASH_NONE;
	w = 0;

	for (i = 0; i < (uint32_t)n; i++) {
		struct cas_entry e = scratch[i];

		if (e.id == MANIFEST_ID_SCENE) {
			plan->has_scene  = 1;
			plan->scene_hash = e.hash;
			continue;
		}
		if (manifest_kind_role(e.kind) != MANIFEST_KIND_ASSET)
			continue;   /* unknown role: ignore, forward-compatible */
		scratch[w++] = e;
	}

	plan->assets      = scratch;
	plan->asset_count = w;
	return plan->has_scene ? 0 : -1;
}

static int32_t plan_has_asset(const struct ingest_plan *plan, uint32_t id)
{
	uint32_t i;

	for (i = 0; i < plan->asset_count; i++) {
		if (plan->assets[i].id == id)
			return 1;
	}
	return 0;
}

/*
 * Apply the decoded asset set to the live catalog: inject/set_data every
 * manifest asset under its stable id, then destroy any authored asset the
 * manifest no longer lists.  Called after the scene has already been
 * swapped, so this is the second half of the atomic reload.
 */
static int32_t apply_assets(struct cas *store, const struct ingest_plan *plan,
			    const struct asset_api *ast,
			    const struct asset_mut_api *mut,
			    const struct memory_api *mem)
{
	uint32_t   i, count, ndrop;
	uint32_t  *drop;
	int32_t    rc;

	for (i = 0; i < plan->asset_count; i++) {
		const struct cas_entry *e = &plan->assets[i];
		struct asset_info        info;
		const void              *bytes;
		uint32_t                 size = 0;

		bytes = cas_get_blob(store, e->hash, &size);
		if (!bytes)
			return -1;

		if (ast->find(e->id, &info) == 0) {
			if (mut->set_data(e->id, bytes, size) != 0)
				return -1;
			continue;
		}

		/*
		 * v1 manifests carry no path (branch_manifest.h keeps the
		 * shape flat: id/kind/hash only), so synthesize a stable,
		 * unique one from the id to satisfy inject()'s path-
		 * uniqueness check.  Entities reference assets by stable id
		 * (render_ref), not path, so this is cosmetic only.
		 */
		{
			char path[48];

			snprintf(path, sizeof(path), "branch-asset:%u", e->id);
			if (mut->inject(e->id, path,
					manifest_kind_asset_type(e->kind),
					bytes, size) != 0)
				return -1;
		}
	}

	/*
	 * Remove authored assets the target manifest no longer lists (e.g.
	 * switching to a branch without asset X removes X).  Snapshot the
	 * current ids first: destroy() swap-removes, so mutating the catalog
	 * while enumerating it by index would skip entries.
	 */
	count = ast->count();
	if (count == 0)
		return 0;

	drop = mem->alloc(count * sizeof(*drop));
	if (!drop)
		return -1;
	ndrop = 0;

	for (i = 0; i < count; i++) {
		struct asset_info info;

		if (ast->info(i, &info) != 0)
			continue;
		if (info.origin != ASSET_ORIGIN_AUTHORED)
			continue;   /* read-only / fetched: not ours to drop */
		if (plan_has_asset(plan, info.id))
			continue;
		drop[ndrop++] = info.id;
	}

	rc = 0;
	for (i = 0; i < ndrop; i++) {
		if (mut->destroy(drop[i]) != 0)
			rc = -1;
	}

	mem->free(drop);
	return rc;
}

int32_t branch_ingest_apply(struct cas *store, struct subsystem_manager *mgr,
			    cas_hash_t manifest)
{
	struct cas_entry           *scratch;
	struct ingest_plan          plan;
	const struct entity_api    *ent;
	const struct asset_api     *ast;
	const struct asset_mut_api *mut;
	const void                 *scene_bytes;
	uint32_t                    scene_size = 0;
	int32_t                     rc;

	if (!store || !store->mem || !mgr)
		return -1;

	ent = subsystem_manager_get_api(mgr, "scene");
	ast = subsystem_manager_get_api(mgr, "asset");
	mut = subsystem_manager_get_api(mgr, "asset_mut");
	if (!ent || !ast || !mut || !ent->ingest_scene_bytes)
		return -1;

	/*
	 * Decode the WHOLE manifest before touching anything live, so a
	 * decode failure leaves the current world/catalog untouched.
	 */
	scratch = store->mem->alloc(CAS_MANIFEST_MAX * sizeof(*scratch));
	if (!scratch)
		return -1;

	if (ingest_decode(store, manifest, scratch, CAS_MANIFEST_MAX,
			  &plan) != 0) {
		store->mem->free(scratch);
		return -1;
	}

	scene_bytes = cas_get_blob(store, plan.scene_hash, &scene_size);
	if (!scene_bytes) {
		store->mem->free(scratch);
		return -1;
	}

	/* Apply as one atomic reload: the world first, then the catalog it
	 * references. */
	if (ent->ingest_scene_bytes(scene_bytes, scene_size) != 0) {
		store->mem->free(scratch);
		return -1;
	}

	rc = apply_assets(store, &plan, ast, mut, store->mem);

	store->mem->free(scratch);
	return rc;
}
