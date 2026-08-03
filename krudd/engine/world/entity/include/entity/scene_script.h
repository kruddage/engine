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
 * scene-material!, scene-script!, scene-name!, and the load pair scene-clear! /
 * scene-build!) plus the authoring pair script-define! / mesh-define!.
 * Idempotent; safe to call before any world is bound, since the primitives only
 * touch a world during a build — the *-define! pair touches none at all.
 */
void scene_script_init(void);

/*
 * Bind the catalog script-define! and mesh-define! register into, for the
 * session. Unlike the world and catalog a build borrows for one call, this pair
 * outlives any build: a project declares its assets while its own source is
 * being evaluated, which is not inside one. Either pointer may be NULL (the
 * *-define! primitives are then inert).
 */
void scene_script_bind_catalog(const struct asset_api *asset,
			       const struct asset_mut_api *mut);

/*
 * Evaluate SRC — a (scene ...) form — against the shared s7 image, spawning its
 * entities into W and resolving each (mesh/material/script "path") clause against
 * ASSET's catalog. Returns the number of entities created, or -1 when the
 * interpreter is unavailable; source that is not a (scene ...) form is logged
 * in the image and counted as zero, so a bad asset costs a frame nothing. A
 * per-entity fault is caught there and skipped too, never taking the whole
 * build down. W and ASSET are borrowed for the call only; no pointer is
 * retained after it returns.
 *
 * Re-entrant: the scene-build! primitive runs a build from inside a call this
 * module already bound, and the inner build restores that outer binding rather
 * than clearing it.
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

/*
 * Call the image's (tick) — a no-argument procedure — with W and ASSET bound
 * for the call, so the scene-* primitives observe the live world from inside
 * it. The per-frame twin of scene_script_call: same set-before / clear-after
 * binding, no argument and no result, because a frame hook has neither.
 *
 * This exists because core's script_tick cannot bind a world: core sits above
 * world/ in the tier order (kruddmake/manifest.scm), so the engine reaches this
 * back down through entity_api's tick_scm rather than core linking this module.
 * A no-op when the image defines no (tick), when the interpreter is down, or
 * when W is NULL — costing one name lookup, exactly what script_tick already
 * paid. W/ASSET are borrowed for the call only.
 */
void scene_script_tick(struct world *w, const struct asset_api *asset);

#endif /* SCENE_SCRIPT_H */
