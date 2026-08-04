/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The lean half of the harness: the world, the stand-in "scene" api over it,
 * the bring-up every project test shares, and the handful of readings each of
 * them used to write for itself.
 *
 * Nothing here names the asset catalog. That is deliberate rather than
 * incidental — this is the translation unit a lean test pulls, and every
 * symbol it references is a symbol that test's binary has to resolve. The
 * catalog lives in project_test_catalog.c for exactly that reason.
 */
#include <project_test/project_test.h>

#include "project_test_impl.h"

#include <abi/entity_api.h>
#include <core/script.h>
#include <core/subsystem_manager.h>
#include <entity/scene_script.h>
#include <entity/world.h>
#include <log/log.h>
#include <memory/memory.h>
#include <project/project_host.h>

#include <stddef.h>
#include <string.h>

/* One world for the process; far too big for a stack frame. */
static struct world w;

static struct subsystem_manager mgr;

/*
 * The catalog handed to every build and dispatch, or NULL. It is held here,
 * in the half that reads it, rather than in the half that produces it: a lean
 * binary then resolves it as an ordinary null pointer and never learns that a
 * catalog is a thing that could have been.
 */
static const struct asset_api *assets;

static int32_t t_build_scene_scm(const char *src)
{
	return scene_script_build(&w, assets, src);
}

static int32_t t_dispatch_scm(const char *fn, int32_t arg)
{
	return scene_script_call(&w, assets, fn, arg);
}

static void t_clear_world(void)
{
	world_reset(&w);
}

/*
 * The three entity_api entries a project's host actually uses, each binding
 * this harness's world exactly as entity_plugin.c binds the live one. There is
 * deliberately no get_selected among them — the host reads the selection from
 * inside the image, through the same (scene-selected) primitive a project
 * would, and never through the vtable.
 */
static const struct entity_api scene_api = {
	.build_scene_scm = t_build_scene_scm,
	.dispatch_scm    = t_dispatch_scm,
	.clear_world     = t_clear_world,
};

static const struct subsystem subsystems[] = {
	{ .name = "scene", .api = &scene_api },
	{ NULL }
};

/*
 * The bring-up, in the one order that works: the image before the catalog
 * (which bakes through it), the catalog before the scene-* primitives and
 * before any project source is evaluated, and the subsystem manager last so
 * the project host finds a "scene" api already standing when it registers.
 * CATALOG_UP is NULL for a lean harness and otherwise answers the catalog it
 * brought up. See project_test_impl.h for why it is a callback.
 */
void project_test_bring_up(const struct asset_api *(*catalog_up)(void))
{
	mem_init();
	log_init();
	script_init();
	if (catalog_up)
		assets = catalog_up();
	scene_script_init();
	world_reset(&w);
	subsystem_manager_init(&mgr, subsystems);
	project_host_plugin_entry(&mgr);
}

void project_test_init(void)
{
	project_test_bring_up(NULL);
}

struct world *project_test_world(void)
{
	return &w;
}

const struct asset_api *project_test_assets(void)
{
	return assets;
}

void project_test_tick(void)
{
	subsystem_manager_tick(&mgr);
}

int32_t project_test_call(const char *fn, int32_t arg)
{
	return scene_script_call(&w, assets, fn, arg);
}

int32_t project_test_build(const char *src)
{
	return scene_script_build(&w, assets, src);
}

int32_t project_test_entity(const char *name)
{
	uint32_t e;

	for (e = 0; e < w.count; e++) {
		const char *n = w.alive[e] ? world_entity_name(&w, e) : NULL;

		if (n && strcmp(n, name) == 0)
			return (int32_t)e;
	}
	return -1;
}
