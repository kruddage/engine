/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The catalog half of the harness — a translation unit of its own, which is
 * the whole point of it.
 *
 * A project that names no asset drives no catalog, and this file is what that
 * costs it: nothing. Its only entry points are project_test_init_with_catalog
 * and project_test_script_tick, so a test that names neither leaves this
 * archive member unpulled, and the seeded built-ins, the mutation api and the
 * entity-script driver never reach its binary. The alternative — a flag on one
 * init, or a second archive — would either make every lean test carry the
 * catalog or make "which harness am I" a build.scm decision. The library is
 * one thing; what a test asks of it is the call it makes.
 *
 * See project_test.h for the surface and project_test_impl.h for why the
 * bring-up arrives here as a callback.
 */
#include <project_test/project_test.h>

#include "project_test_impl.h"

#include "asset.h"
#include "entity_script.h"

#include <abi/asset_api.h>
#include <entity/scene_script.h>

/*
 * The real catalog, assembled from asset.h exactly the way asset_plugin.c
 * assembles it for the subsystem manager. Read side and mutation side both:
 * a project declares its own meshes, materials and scripts through the
 * *-define! trio, which writes through the second and resolves through the
 * first — material-define! reads the shader it packs against back out of the
 * same catalog it is registering into.
 */
static const struct asset_api catalog_api = {
	.count    = asset_catalog_count,
	.info     = asset_catalog_info,
	.describe = asset_catalog_describe,
	.find     = asset_catalog_find,
	.get_data = asset_catalog_get_data,
};

static const struct asset_mut_api mut_api = {
	.create   = asset_mut_create,
	.set_data = asset_mut_set_data,
	.destroy  = asset_mut_destroy,
	.set_decl = asset_mut_set_decl,
	.inject   = asset_mut_inject,
};

/*
 * Seed the catalog, register the entity-* primitives a (script ...) clause
 * calls, and bind the pair the *-define! trio registers into. Run from the
 * middle of the bring-up: after the image, because seeding bakes through it,
 * and before any project source is evaluated, because a project's top-level
 * *-define! calls are what put its own assets in the catalog.
 */
static const struct asset_api *catalog_up(void)
{
	asset_init();
	entity_script_init();
	scene_script_bind_catalog(&catalog_api, &mut_api);
	return &catalog_api;
}

void project_test_init_with_catalog(void)
{
	project_test_bring_up(catalog_up);
}

void project_test_script_tick(float t)
{
	entity_script_tick(project_test_world(), &catalog_api, t);
}
