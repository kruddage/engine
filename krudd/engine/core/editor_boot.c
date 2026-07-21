/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * editor_boot — the native render-cluster boot (see editor_boot.h and #675).
 *
 * engine.c's plugin_table + finish_plugin_boot live behind #ifdef
 * __EMSCRIPTEN__: on the web the module registers the whole plugin cluster in
 * dependency order once the WebGPU device lands. Natively there was no
 * equivalent — the native harnesses registered only log/memory and the backend,
 * and drew straight through the gpu_api vtable. This is the missing seam: the
 * same registration order, as data, so the offscreen and Qt harnesses share it.
 *
 * The order mirrors engine.c's plugin_table exactly for the plugins that are
 * not browser-bound:
 *
 *   asset          seeds the built-in catalog (meshes, shaders, materials,
 *                  scripts, textures) — its native path has no fetch/IndexedDB.
 *   edit           the undo/redo journal entity resolves as "edit" (optional,
 *                  registered here so scene edits are journalled as on the web).
 *   entity         the "scene" api: the world, entity scripts, scene DSL.
 *   frame_graph    the pass graph scene_renderer records into; resolves
 *                  "renderer" at entry, so the backend must already be up.
 *   scene_renderer resolves frame_graph + scene + asset at entry and the device
 *                  in its init, where it also seeds the demo scene.
 *
 * Deliberately NOT registered here (the browser-bound / interactive layer that
 * is #676's authoring surface, not #675's viewport):
 *   audio      — Web Audio; the scriptnode backend assumes an AudioContext.
 *   viewport   — click-to-pick + aspect sync, driven by the kruddgui pointer.
 *   kruddgui   — the canvas overlay UI.
 *   tictactoe / chess — the games, which register on the launcher.
 * The camera-aspect half of the viewport bridge is done directly by the caller
 * (the "camera" api's set_viewport, once per frame with the window size), so a
 * native scene is framed correctly without the kruddgui pointer.
 */
#include "editor_boot.h"

#include "subsystem_manager.h"

/*
 * The unconditional native entry points (each plugin exposes a unique
 * <name>_plugin_entry that registers its subsystem — the same symbols
 * engine.c's plugin_table names).
 */
extern void asset_plugin_entry(struct subsystem_manager *mgr);
extern void edit_plugin_entry(struct subsystem_manager *mgr);
extern void entity_plugin_entry(struct subsystem_manager *mgr);
extern void fg_plugin_entry(struct subsystem_manager *mgr);
extern void scene_renderer_plugin_entry(struct subsystem_manager *mgr);

void editor_boot_cluster(struct subsystem_manager *mgr)
{
	/*
	 * subsystem_manager_register() runs each subsystem's init at register
	 * time, so this call order IS the boot order: every api a plugin resolves
	 * must already be registered when its entry point runs. asset before
	 * entity (entity reads the catalog), the backend before frame_graph
	 * (frame_graph resolves "renderer"), and all of frame_graph/scene/asset
	 * before scene_renderer (it resolves the three at entry).
	 */
	asset_plugin_entry(mgr);
	edit_plugin_entry(mgr);
	entity_plugin_entry(mgr);
	fg_plugin_entry(mgr);
	scene_renderer_plugin_entry(mgr);
}
