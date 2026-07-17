/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SCENE_SCRIPT_H
#define SCENE_SCRIPT_H

#include "world.h"
#include "asset_api.h"

#include <stdint.h>

/*
 * scene-script — builds a live world from a (scene NAME (entity ...) ...) Scheme
 * form, the declarative twin of the (mesh ...) / (script ...) DSLs. The form is
 * plain S7 source (see core/scene_script.scm): scene_script_build reads it and
 * calls the scene-* host primitives registered here to spawn and bind entities,
 * so a scene is authored and loaded exactly the way every other asset is —
 * source text evaluated against the shared image, not a bespoke binary loader.
 *
 * This is the engine's generic "build a scene from Scheme" capability; a game
 * (engine/games/<name>) is that vocabulary plus a scene form and its logic. No
 * game-specific knowledge lives here.
 */

/*
 * Register the scene-* host primitives (scene-spawn, scene-xform!, scene-mesh!,
 * scene-material!, scene-script!, scene-name!). Idempotent; safe to call before
 * any world is bound, since the primitives only touch a world during a build.
 */
void scene_script_init(void);

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

#endif /* SCENE_SCRIPT_H */
