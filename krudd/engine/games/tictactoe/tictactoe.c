/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * tictactoe — the first built-in game. It is deliberately thin: the plugin owns
 * no engine C, it just hands the engine its scene. At register time it resolves
 * the "scene" api and asks it to build the embedded (scene ...) form (scene.scm)
 * into the live world, proving a game ships as authored data on top of the
 * generic scene builder. Board rules, input and turn state arrive in later
 * slices as more .scm + a game tick; this file stays the boot seam.
 */
#include "entity_api.h"
#include "subsystem_manager.h"

#include "tictactoe_scene_scm.h"

#include <stddef.h>
#include <stdint.h>

/* Resolved before register() so the subsystem init can reach the world. */
static const struct entity_api *g_scene;

static void tictactoe_init(void)
{
	if (g_scene && g_scene->build_scene_scm)
		g_scene->build_scene_scm(TICTACTOE_SCENE_SCM);
}

static const struct subsystem tictactoe_desc = {
	.name = "tictactoe",
	.init = tictactoe_init,
};

void tictactoe_plugin_entry(struct subsystem_manager *mgr)
{
	/* The "scene" api is the entity plugin, registered before this one. */
	g_scene = subsystem_manager_get_api(mgr, "scene");
	subsystem_manager_register(mgr, &tictactoe_desc);
}
