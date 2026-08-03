/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SCENE_SCRIPT_H
#define SCENE_SCRIPT_H

#include <entity/world.h>
#include <abi/asset_api.h>

#include <stdint.h>

/*
 * scene-script — builds a live world from a (scene NAME (entity ...) ...) Scheme
 * form, the declarative twin of the (mesh ...) / (script ...) DSLs. The form is
 * plain S7 source (see scene_script.scm): scene_script_build reads it and
 * calls the scene-* host primitives registered here to spawn and bind entities,
 * so a scene is authored and loaded exactly the way every other asset is —
 * source text evaluated against the shared image, not a bespoke binary loader.
 *
 * This is the engine's generic "build a scene from Scheme" capability; a game
 * (engine/game/<name>) is that vocabulary plus a scene form and its logic. No
 * game-specific knowledge lives here.
 */

/*
 * Register the scene-* host primitives (scene-spawn, scene-xform!, scene-mesh!,
 * scene-material!, scene-script!, scene-name!) plus script-define!. Idempotent;
 * safe to call before any world is bound, since the primitives only touch a
 * world during a build — script-define! touches none at all.
 */
void scene_script_init(void);

/*
 * Bind the catalog script-define! registers into, for the session. Unlike the
 * world and catalog a build borrows for one call, this pair outlives any build:
 * a project declares its assets while its own source is being evaluated, which
 * is not inside one. Either pointer may be NULL (script-define! is then inert).
 */
void scene_script_bind_catalog(const struct asset_api *asset,
			       const struct asset_mut_api *mut);

/*
 * Evaluate SRC — a (scene ...) form — against the shared s7 image, spawning its
 * entities into W and resolving each (mesh/material/script "path") clause against
 * ASSET's catalog. Returns the number of entities created, or -1 when the
 * interpreter is unavailable or SRC is not a (scene ...) form. A per-entity fault
 * is caught in the image and skipped, never taking the whole build down. W and
 * ASSET are borrowed for the call only; no pointer is retained after it returns.
 */
int32_t scene_script_build(struct world *w, const struct asset_api *asset,
			   const char *src);

/*
 * Invoke image function FN (an integer -> integer procedure) with ARG, W and
 * ASSET bound for the call so the scene-* primitives can spawn and mutate — the
 * runtime, event-driven twin of scene_script_build. This is how stateful game
 * rules living in the image respond to a click: place a mark, advance the turn.
 * Returns FN's integer result (0 if it returns a non-integer), or -1 when the
 * interpreter is unavailable or FN is undefined. W/ASSET are borrowed for the
 * call only.
 */
int32_t scene_script_call(struct world *w, const struct asset_api *asset,
			  const char *fn, int32_t arg);

#endif /* SCENE_SCRIPT_H */
