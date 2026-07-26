/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef KRUDD_EDITOR_BOOT_H
#define KRUDD_EDITOR_BOOT_H

struct subsystem_manager;

/*
 * Stand up the engine's render cluster natively — the same asset -> entity ->
 * frame_graph -> scene_renderer chain engine.c's finish_plugin_boot registers
 * on the web, minus the browser-bound layer (the kruddgui canvas overlay, the
 * IndexedDB/fetch asset origins, the games and the live REPL). It exists so the
 * offscreen (krudd_native) and windowed (krudd_qt) native harnesses boot the
 * real scene renderer through one sequence rather than each hand-rolling — and
 * drifting from — the registration order.
 *
 * PRECONDITIONS, in this order, before calling:
 *   1. subsystem_manager_init() has run with at least "log", "memory" and
 *      "stats" in the static table (entity reads the frame delta off "stats").
 *   2. script_init() has run: meshes, shaders, procedural textures and entity
 *      scripts are all lowered through the process-global s7 image, so nothing
 *      in the cluster draws without it.
 *   3. The "renderer" backend (renderer_webgpu or renderer_null) is registered
 *      AND its device is ready — scene_renderer_init builds its pipelines
 *      against the device at register time.
 *
 * On return the cluster is live: scene_renderer_init has seeded the built-in
 * demo scene (the same floor/box/sphere/pyramid/rook the web canvas shows on
 * load) into the world, and a subsystem_manager_tick() draws it.
 */
void editor_boot_cluster(struct subsystem_manager *mgr);

#endif /* KRUDD_EDITOR_BOOT_H */
