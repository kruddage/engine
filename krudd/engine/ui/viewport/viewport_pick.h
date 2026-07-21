/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VIEWPORT_PICK_H
#define VIEWPORT_PICK_H

#include <stdint.h>

struct world;
struct mat4;
struct asset_api;
struct memory_api;

/*
 * Click-to-pick: cast a ray from viewport pixel (sx, sy) — a top-left origin,
 * in a (vw, vh) pixel viewport — through the world and return the live render
 * entity whose mesh it strikes nearest the camera, or -1 on a miss (or on a
 * NULL argument).
 *
 * view_proj is the live camera's view·projection: the exact matrix the forward
 * pass draws with, so a click lands on the pixels a mesh was drawn to. Every
 * alive COMPONENT_RENDER entity is a candidate; its geometry is generated on
 * demand from the asset catalog's mesh source (asset->get_data on render_ref)
 * with this entity's mesh-param override, so a resized box's hit-box matches the
 * box that was drawn, not the default. `mem` is the scratch allocator for that
 * per-click mesh gen. Brute force over every triangle of every render entity —
 * the world caps at WORLD_MAX_ENTITIES and the meshes are tiny.
 *
 * The shared copy of the raycast so the wasm viewport overlay (which passes the
 * kruddgui viewport + camera) and the native Qt shell (which passes the window
 * size + camera) never drift.
 */
int32_t viewport_pick_entity(const struct world *w,
			     const struct mat4 *view_proj,
			     float sx, float sy, float vw, float vh,
			     const struct asset_api *asset,
			     const struct memory_api *mem);

#endif /* VIEWPORT_PICK_H */
