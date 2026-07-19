/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * chess — a built-in game that is, for now, a set to look at rather than a game
 * to play: the standard opening position, staged to show off the engine's lathed
 * meshes (abi/builtin_mesh_scripts.h) and its PBR materials. Like tictactoe the
 * plugin stays thin — it owns no geometry and no rules, only the wiring — but it
 * is thinner still, because this slice has no interaction: no board state, no
 * turn order, no move legality. At register time it offers "Chess" on the
 * launcher; when chosen, chess_load clears the world and builds the scene form
 * (games/chess/scene.scm) through the shared scene builder. Full chess rules —
 * selection, legal moves, check — are a later slice; they would live in a
 * rules.scm loaded here, exactly the way tictactoe's do.
 */
#include "entity_api.h"
#include "subsystem_manager.h"
#include "game.h"

#include "chess_scene_scm.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __EMSCRIPTEN__
/* Editor-chrome toggle (plugin_abi.c, main module). */
void krudd_set_editor_chrome(int on);
#endif

/* Resolved before register() so the load callback can reach the world. */
static const struct entity_api *g_scene;

/* Launcher entry: clear whatever was showing and build the chess scene. */
static void chess_load(void)
{
	if (!g_scene)
		return;
#ifdef __EMSCRIPTEN__
	/*
	 * A set to view, not a scene to edit: drop the editor chrome (panels and
	 * the selection gizmo) so the board fills the frame. There is no
	 * click interaction yet, so nothing here depends on the ray pick.
	 */
	krudd_set_editor_chrome(0);
#endif
	if (g_scene->clear_world)
		g_scene->clear_world();
	if (g_scene->build_scene_scm)
		g_scene->build_scene_scm(CHESS_SCENE_SCM);
}

void chess_plugin_entry(struct subsystem_manager *mgr)
{
	/*
	 * Like the demo, chess owns no per-frame subsystem — it has no tick, so
	 * it just resolves the "scene" api (the entity plugin, up before this
	 * one) and offers itself on the launcher. The board is built by
	 * chess_load when the game is chosen, not at boot.
	 */
	g_scene = subsystem_manager_get_api(mgr, "scene");
	game_register("Chess", chess_load);
}
