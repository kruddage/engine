/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "branch_serialize.h"
#include "branch_manifest.h"

#include "cas.h"
#include "entity_api.h"
#include "asset_api.h"
#include "memory_api.h"
#include "subsystem_manager.h"

#include <stddef.h>

/*
 * One already-gathered authored asset: raw bytes plus the identity needed to
 * place it in the manifest (branch_manifest.h).  `bytes`/`size` are borrowed
 * from the live catalog; the pure core below only reads them.
 */
struct branch_serialize_asset {
	uint32_t    id;
	int32_t     type;
	const void *bytes;
	uint32_t    size;
};

/* Undo cas_put_blob for the first n entries of ents (best-effort cleanup on a
 * failure partway through a capture, so an aborted capture does not pin
 * blobs forever). */
static void serialize_release(struct cas *store, const struct cas_entry *ents,
			      uint32_t n)
{
	uint32_t i;

	for (i = 0; i < n; i++)
		cas_drop_blob(store, ents[i].hash);
}

/*
 * Pure, native-testable core: given canonical scene bytes and a flat list of
 * authored asset sources, write every blob to `store` and assemble the
 * manifest per branch_manifest.h.  Depends only on cas.h (no live subsystem
 * apis), so it is exercised directly by a standalone test.
 *
 * Returns 0 and writes *out on success; -1 on bad args, too many assets, or a
 * cas failure (OOM / hash collision), in which case *out is untouched and any
 * blobs already written by this call are released.
 */
static int32_t branch_serialize_build_manifest(
	struct cas *store,
	const void *scene_bytes, uint32_t scene_size,
	const struct branch_serialize_asset *assets, uint32_t asset_count,
	cas_hash_t *out)
{
	struct cas_entry *ents;
	uint32_t          n = 0;
	uint32_t          i;
	int32_t           rc;

	if (!store || !store->mem || !out)
		return -1;
	if ((scene_size && !scene_bytes) || (asset_count && !assets))
		return -1;
	/* The scene entry takes one of the CAS_MANIFEST_MAX slots. */
	if (asset_count >= CAS_MANIFEST_MAX)
		return -1;

	ents = store->mem->alloc((size_t)(asset_count + 1) * sizeof(*ents));
	if (!ents)
		return -1;

	ents[n].id   = MANIFEST_ID_SCENE;
	ents[n].kind = MANIFEST_KIND_SCENE;
	if (cas_put_blob(store, scene_bytes, scene_size, &ents[n].hash) != 0) {
		store->mem->free(ents);
		return -1;
	}
	n++;

	for (i = 0; i < asset_count; i++) {
		ents[n].id   = assets[i].id;
		ents[n].kind = manifest_kind_asset(assets[i].type);
		if (cas_put_blob(store, assets[i].bytes, assets[i].size,
				 &ents[n].hash) != 0) {
			serialize_release(store, ents, n);
			store->mem->free(ents);
			return -1;
		}
		n++;
	}

	rc = cas_put_manifest(store, ents, n, out);
	if (rc != 0)
		serialize_release(store, ents, n);
	store->mem->free(ents);
	return rc;
}

/*
 * Glue: gather the scene bytes and the authored catalog from the live
 * subsystems, then hand them to the pure core above.  See branch_serialize.h
 * for the full contract.
 */
int32_t branch_serialize_capture(struct cas *store,
				 struct subsystem_manager *mgr,
				 cas_hash_t *out)
{
	const struct entity_api        *scene;
	const struct asset_api         *asset;
	struct branch_serialize_asset  *assets = NULL;
	void                            *scene_bytes;
	uint32_t                         scene_size = 0;
	uint32_t                         count, i, n = 0;
	int32_t                          rc = -1;

	if (!store || !store->mem || !mgr || !out)
		return -1;

	scene = subsystem_manager_get_api(mgr, "scene");
	asset = subsystem_manager_get_api(mgr, "asset");
	if (!scene || !scene->export_scene_bytes)
		return -1;
	if (!asset || !asset->count || !asset->info || !asset->get_data)
		return -1;

	scene_bytes = scene->export_scene_bytes(&scene_size);
	if (!scene_bytes)
		return -1;

	count = asset->count();
	if (count) {
		assets = store->mem->alloc((size_t)count * sizeof(*assets));
		if (!assets) {
			store->mem->free(scene_bytes);
			return -1;
		}
	}

	for (i = 0; i < count; i++) {
		struct asset_info info;
		const void        *data;
		uint32_t           size = 0;

		if (asset->info(i, &info) != 0)
			goto out;
		/*
		 * Only AUTHORED assets are project state (mirrors what the
		 * persistence layer saves and what ingest destroys on switch).
		 * Fetched/built-in content is re-acquired by path, not branched.
		 */
		if (info.origin != ASSET_ORIGIN_AUTHORED)
			continue;

		data = asset->get_data(info.id, &size);
		if (!data) {
			/* A loaded, non-read-only entry always resolves; a
			 * genuinely empty blob is the only legitimate NULL. */
			if (info.size != 0)
				goto out;
			size = 0;
		}

		assets[n].id    = info.id;
		assets[n].type  = info.type;
		assets[n].bytes = data;
		assets[n].size  = size;
		n++;
	}

	rc = branch_serialize_build_manifest(store, scene_bytes, scene_size,
					     assets, n, out);

out:
	if (assets)
		store->mem->free(assets);
	store->mem->free(scene_bytes);
	return rc;
}
