/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * demo — the procedural showcase, registered on the launcher as the default
 * scene. It has no rules and no tick: loading it just clears the world and builds
 * the demo scene (see demo.scm). The thinnest possible "game" — proof that a
 * launcher entry is only a name plus a scene to build.
 */
#include "entity_api.h"
#include "subsystem_manager.h"
#include "game.h"

#include "demo_scene_scm.h"

#include <stddef.h>

/* Resolved at plugin entry so the load callback can reach the world. */
static const struct entity_api *g_scene;

static void demo_load(void)
{
	if (!g_scene)
		return;
	if (g_scene->clear_world)
		g_scene->clear_world();
	if (g_scene->build_scene_scm)
		g_scene->build_scene_scm(DEMO_SCENE_SCM);
}

void demo_plugin_entry(struct subsystem_manager *mgr)
{
	g_scene = subsystem_manager_get_api(mgr, "scene");
	game_register("Procedural Demo", demo_load);
}
