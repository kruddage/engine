/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VIEWPORT_PICK_H
#define VIEWPORT_PICK_H

#include <entity/world.h>
#include <stdint.h>

struct world;
struct mat4;
struct asset_api;
struct memory_api;

/*
 * The raycast, and the two ways a caller can name the ray.
 *
 * This is a PUBLIC header rather than a private one (#996) because the ray no
 * longer has to come from a pixel. A module ABOVE ui/ that already holds a
 * world-space ray — an XR controller is one: a pose and a forward axis — calls
 * viewport_pick_ray() and skips the unprojection entirely. The dependency runs
 * downward only: this module never learns who is pointing, and nothing here
 * mentions a headset, a session or a controller.
 */

/*
 * Cast a world-space ray through the world and return the live render entity
 * whose mesh it strikes NEAREST THE ORIGIN, or -1 on a miss (or a NULL
 * argument, or a degenerate direction).
 *
 * origin and dir are metres and a direction in the engine's world space; dir
 * need not be unit length, but the t it is measured in scales with it, so the
 * nearest hit is the nearest hit either way. Every alive COMPONENT_RENDER
 * entity is a candidate; its geometry is generated on demand from the asset
 * catalog's mesh source (asset->get_data on render_ref) with this entity's
 * mesh-param override, so a resized box's hit-box matches the box that was
 * drawn, not the default. `mem` is the scratch allocator for that per-pick mesh
 * gen. Brute force over every triangle of every render entity — the world caps
 * at WORLD_MAX_ENTITIES and the meshes are tiny.
 *
 * ignore lists ignore_count entity ids that are not candidates, and it exists
 * because a world-space pointer can have a BODY IN THE WORLD in a way a screen
 * pointer never could: a ray drawn as a rod along itself is struck by itself at
 * t ~ 0 and wins every pick. Pass NULL/0 when the ray has no such body. Ids
 * that are not live entities are simply never matched, so a caller may hold
 * stale ones across a scene change without checking them first.
 */
int32_t viewport_pick_ray(const struct world *w,
			  const float origin[3], const float dir[3],
			  const int32_t *ignore, int32_t ignore_count,
			  const struct asset_api *asset,
			  const struct memory_api *mem);

/*
 * Click-to-pick: the same raycast, from viewport pixel (sx, sy) — a top-left
 * origin, in a (vw, vh) pixel viewport.
 *
 * view_proj is the live camera's view·projection: the exact matrix the forward
 * pass draws with, so a click lands on the pixels a mesh was drawn to. This is
 * ray_from_screen followed by viewport_pick_ray and nothing else — the
 * unprojection is the ONLY thing the screen-space path adds, which is what
 * makes a controller's ray a caller of the same code rather than a second copy
 * of it.
 *
 * The one copy of the raycast: the wasm viewport overlay passes the kruddgui
 * viewport + camera, and the GPU-free unit test passes a synthetic pair, so the
 * thing under test is the thing that ships.
 */
int32_t viewport_pick_entity(const struct world *w,
			     const struct mat4 *view_proj,
			     float sx, float sy, float vw, float vh,
			     const struct asset_api *asset,
			     const struct memory_api *mem);

#endif /* VIEWPORT_PICK_H */
