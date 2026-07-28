/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SCENE_SAVE_H
#define SCENE_SAVE_H

#include "asset_api.h"
#include "world.h"

#include <stdint.h>

/*
 * scene-save — the live world, written back out as the `(scene ...)` form
 * scene_script.scm already reads (#947).
 *
 * ## Why this is a writer and not a format
 *
 * The engine could load a scene long before it could save one: `load_scene`
 * ingests a .scene asset and `build_scene_scm` evaluates a `(scene ...)` form,
 * and both predate any editor that would want to write one back. #947 requires
 * the document to round-trip and requires it to go through "whatever path the
 * engine already persists with, not a second one" — so this adds the missing
 * direction of an existing path rather than inventing a serialization format
 * beside it.
 *
 * The output is therefore not a new file type. It is text that
 * `entity_api.build_scene_scm` accepts, which is the same text a human can
 * author by hand, which is the property scene_script.scm calls "source text is
 * the asset". A save the engine cannot read back is not a save.
 *
 * ## The one clause this adds to the form
 *
 * `(rotate X Y Z)` is intrinsic X-Y-Z Euler degrees, and the world stores a
 * quaternion. Going quaternion -> Euler -> quaternion is not the identity: it
 * is ambiguous at gimbal lock and lossy everywhere else, which would make
 * "round-trips unchanged" false in a way that only shows up on the poses
 * nobody thinks to test.
 *
 * So this writes `(quat X Y Z W)` — the stored value, verbatim — and
 * scene_script.scm learns to read it. `(rotate ...)` stays exactly as it was,
 * because hand-authored scenes want degrees and a generated one does not.
 *
 * ## Costs, stated
 *
 * Finding an entity's children is a scan of the world, so writing the whole
 * tree is O(entities^2). That is deliberate: saving is a cold path measured in
 * milliseconds once, and an index built here would be a second source of truth
 * about the hierarchy for the rest of the tree to keep honest.
 *
 * Nesting recurses, bounded by SCENE_SAVE_MAX_DEPTH. A hierarchy deeper than
 * that fails the save loudly rather than overflowing the stack of a wasm build
 * with a 64KB one.
 */

/*
 * How deep a nesting this writes before giving up. A level's entity tree is a
 * handful of levels; 64 is far past anything a designer builds and far short
 * of anything that threatens the stack.
 */
#define SCENE_SAVE_MAX_DEPTH 64

/*
 * Write `w` as a `(scene NAME ...)` form into `out`.
 *
 * `assets` resolves a mesh/material/script ref back to the catalog path the
 * form binds by; when it is NULL, or an entry has gone, the binding clause is
 * omitted and the rest of the entity still writes. `name` is the scene's
 * symbol and defaults to "scene" when NULL or empty.
 *
 * Returns the number of bytes written, not counting the NUL, or -1 when the
 * arguments are unusable, the text does not fit `cap`, or the hierarchy is
 * deeper than SCENE_SAVE_MAX_DEPTH. On -1 the contents of `out` are
 * unspecified: a truncated scene form is a scene that loads back as a
 * different, smaller world, which is worse than a failed save.
 */
int32_t scene_save_write(const struct world *w, const struct asset_api *assets,
			 const char *name, char *out, int32_t cap);

#endif /* SCENE_SAVE_H */
