/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * viewport — the game viewport's bridge to the scene.
 *
 * Two jobs, both game-critical and both run once per kruddgui tick:
 *
 *   1. Keep the camera's projection aspect matched to the live canvas
 *      (camera_api's set_viewport). Without it the projection is stuck at its
 *      authored default aspect, so any canvas that isn't that ratio stretches
 *      the scene — the "fishbowl".
 *   2. Turn a click on the bare viewport into an entity pick (raycast the
 *      pointer against entity meshes, hand the hit to entity_api's
 *      set_selected). The built-in games read the selection edge each tick:
 *      tic-tac-toe places on the picked cell, chess picks up and moves the
 *      picked piece. With no pick, no click ever reaches a game.
 *
 * Both were the game-facing half of the editor overlay that lived in the
 * removed kruddboard.cpp (#661): its draw_viewport_tools synced the aspect and
 * ran click-to-pick every tick, under the transform gizmo it also drew. The
 * gizmo was editor chrome (gone with the editor); the aspect sync and the pick
 * were not, but removing kruddboard took the whole overlay — and with it both —
 * away. This restores exactly those two behaviors as a small standalone plugin:
 * no gizmo, no panels, no undo/redo, none of the editor.
 *
 * It registers a kruddgui overlay (kruddgui_api) rather than doing the work in
 * its own tick, so it runs at the one point in the frame the pointer's one-frame
 * click edge is still live (kruddgui clears it right after the overlays run) and
 * on kruddgui's already-routed pointer (a tap on a panel is claimed there and
 * never reaches the pick). The raycast is the same one kruddboard used —
 * ray_from_screen then ray_tri_intersect over each render entity's
 * mesh_script_generate geometry, with its per-entity mesh-param override so the
 * hit-box matches the box that was drawn.
 *
 * wasm-only: native builds host no games, no canvas, and no kruddgui pointer.
 */

#include "subsystem.h"
#include "subsystem_manager.h"
#include "log_api.h"
#include "log_level.h"

static const struct log_api           *g_log;
static const struct subsystem_manager *g_mgr;

#ifdef __EMSCRIPTEN__
#include "kruddgui_api.h"
#include "camera_api.h"
#include "entity_api.h"
#include "asset_api.h"
#include "memory_api.h"
#include "math_types.h"
#include "viewport_pick.h"

#include <stdint.h>

static const struct kruddgui_api *g_kgui;   /* NULL until kruddgui is up */
static const struct camera_api   *g_camera;
static const struct entity_api   *g_scene;
static const struct asset_api    *g_asset;
static const struct memory_api   *g_mem;    /* for the click-to-pick mesh gen */
static int                        g_registered;

/*
 * Click-to-pick: cast a ray from the clicked pixel and return the live render
 * entity whose mesh it strikes nearest the camera, or -1 when the ray misses
 * everything. The raycast itself is the shared viewport_pick_entity (#697); here
 * we feed it the exact view·projection the renderer draws with and the kruddgui
 * viewport size, so a click lands on the pixels a mesh was drawn to.
 */
static int32_t pick_entity_at(float sx, float sy)
{
	const struct world *w;
	struct mat4         vp;
	float               dw, dh;

	if (!g_camera || !g_scene || !g_asset || !g_mem || !g_kgui)
		return -1;
	w = g_scene->get_world();
	if (!w)
		return -1;

	g_camera->get_view_proj(&vp);
	g_kgui->viewport(&dw, &dh);
	return viewport_pick_entity(w, &vp, sx, sy, dw, dh, g_asset, g_mem);
}

/*
 * The kruddgui overlay: sync the camera aspect to this tick's canvas, then, on a
 * click that reached the bare viewport, select the entity under it (or clear the
 * selection on a miss, so a game's edge detection resets). A tap kruddgui routed
 * to a panel is not our click — pointer_clicked reports only the unclaimed
 * pointer, and over_ui guards the rest — so this never steals a UI gesture.
 */
static void viewport_overlay(void *userdata)
{
	float px, py;

	(void)userdata;
	if (!g_kgui)
		return;

	{
		float dw, dh;

		g_kgui->viewport(&dw, &dh);
		if (g_camera && g_camera->set_viewport)
			g_camera->set_viewport(dw, dh);
	}

	if (!g_scene || !g_kgui->pointer_clicked())
		return;
	g_kgui->pointer(&px, &py);
	if (g_kgui->over_ui(px, py))
		return;
	g_scene->set_selected(pick_entity_at(px, py));
}
#endif /* __EMSCRIPTEN__ */

static void viewport_init(void)
{
	if (g_log)
		g_log->write(LOG_LEVEL_INFO, "viewport: init");
}

static void viewport_tick(void)
{
#ifdef __EMSCRIPTEN__
	if (g_registered)
		return;

	/*
	 * kruddgui registers after this plugin, so its api is not up at
	 * plugin_entry. Resolve it lazily and register the overlay once it is
	 * (retry next tick until then).
	 */
	g_kgui = subsystem_manager_get_api(g_mgr, "kruddgui");
	if (!g_kgui)
		return;

	g_kgui->register_overlay(viewport_overlay, NULL);
	g_registered = 1;
	if (g_log)
		g_log->write(LOG_LEVEL_INFO, "viewport: overlay registered");
#endif
}

static void viewport_shutdown(void)
{
	if (g_log)
		g_log->write(LOG_LEVEL_INFO, "viewport: shutdown");
}

static const struct subsystem desc = {
	.name     = "viewport",
	.init     = viewport_init,
	.tick     = viewport_tick,
	.shutdown = viewport_shutdown,
};

void viewport_plugin_entry(struct subsystem_manager *mgr)
{
	g_mgr = mgr;
	g_log = subsystem_manager_get_api(mgr, "log");
#ifdef __EMSCRIPTEN__
	/* Services this plugin picks against; all registered before it. */
	g_camera = subsystem_manager_get_api(mgr, "camera");
	g_scene  = subsystem_manager_get_api(mgr, "scene");
	g_asset  = subsystem_manager_get_api(mgr, "asset");
	g_mem    = subsystem_manager_get_api(mgr, "memory");
#endif
	subsystem_manager_register(mgr, &desc);
}
