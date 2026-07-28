/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BRIDGE_PARAMS_H
#define BRIDGE_PARAMS_H

#include "asset_api.h"
#include "entity_api.h"
#include "script.h"

#include <stdint.h>

/*
 * bridge-params — an entity's editable parameters, resolved (#951).
 *
 * ## Why this exists, and why it is not a new vocabulary
 *
 * #951's whole point is that the inspector *derives* its controls from the
 * asset rather than being hand-written beside it: `(edit color)` and
 * `(edit range 0 1)` already exist in every asset script, GLSL and WGSL
 * emission both ignore them on purpose, and #813 called that the smallest and
 * highest-leverage finding in the tree.
 *
 * The engine already reads that vocabulary. `script-params-form` in the s7
 * image parses a `(params ...)` clause into name, type, offset, size,
 * components, the three edit fields and the default — once, shared by mesh,
 * texture, sound, script and shader — and core/script.c exposes five C entry
 * points over it, all reporting the same `struct shader_param`.
 *
 * So this module invents nothing. It answers one question the editor cannot
 * answer for itself: **for this entity, which parameter blocks are in play,
 * what does each declare, and what value is each field actually at right now.**
 *
 * The last part is the one that has to live here rather than in TypeScript. A
 * field's current value is "the entity's override where it is present and long
 * enough, else the declared default" — the exact rule mesh_script_generate and
 * its siblings apply when they *use* the value. Reimplementing it across the
 * boundary would mean the inspector showing one number while the renderer drew
 * another, which is the failure mode #944's Q1 rejected a mirrored store to
 * avoid.
 *
 * ## Slots
 *
 * Three, matching the three refs an entity carries that lead to a source with a
 * `(params ...)` clause:
 *
 *   mesh      render_ref   -> the mesh script's own params
 *   material  material_ref -> the *shader's* Material block, via the material's
 *                            wire form, whose first u32 is the shader ref
 *   script    script_ref   -> the entity script's params
 *
 * **Texture params are deliberately not here.** The world carries a texture
 * override column, but the texture an entity draws with is named inside its
 * material's wire form rather than by a ref on the entity, and that form varies
 * by material kind. Resolving it means decoding a format this module has no
 * other reason to know. It is a real gap, it is stated in the PR rather than
 * discovered later, and it costs one slot in a panel rather than a redesign:
 * the shape below already carries a slot name, so a fourth is an addition.
 */

/* Fields in one block. Past this the block reports what fit and says so. */
#define BRIDGE_MAX_PARAM_FIELDS 32

/* Blocks per entity — one per slot above. */
#define BRIDGE_MAX_PARAM_BLOCKS 3

struct bridge_param_block {
	/* Interned literal: "mesh", "material" or "script". */
	const char	*slot;
	/* Catalog id of the asset whose params these are. Never 0 here. */
	uint32_t	 asset;
	/* Catalog path, or NULL when the entry has gone. */
	const char	*path;
	/* Packed size of the whole block, per the source's own packing. */
	uint32_t	 total;
	int32_t		 count;
	/*
	 * Whether the entity carries its own bytes for this block. False means
	 * every value below is a declared default — which the editor shows
	 * differently, because "this is the default" and "this was set to the
	 * default" are different facts about a project.
	 */
	int32_t		 overridden;
	/* True when the declaration had more fields than fit. */
	int32_t		 truncated;

	struct shader_param	fields[BRIDGE_MAX_PARAM_FIELDS];
	/*
	 * Resolved current value, up to four components per field.
	 *
	 * Floats even for an `int` field: the wire carries numbers, JSON has one
	 * number type, and a client that had to know which fields were secretly
	 * integers in order to parse them would be deriving less than it looks.
	 * The type string travels beside it, so a control that must round still
	 * can.
	 */
	float			values[BRIDGE_MAX_PARAM_FIELDS][4];
};

/*
 * Fill `out` with every parameter block entity `id` has.
 *
 * Returns the number of blocks written (0 when the entity has no parameterized
 * asset bound, which is the common case and not an error), or -1 when `id` is
 * not a live entity or the services needed are absent.
 *
 * A slot whose asset has gone from the catalog, or whose source does not parse,
 * is skipped rather than reported empty: an inspector showing a block with no
 * fields reads as "this asset has no parameters", which is a different and
 * wrong claim.
 */
int32_t bridge_params_collect(const struct entity_api *entity,
			      const struct asset_api *asset, int32_t id,
			      struct bridge_param_block *out, int32_t max);

/*
 * Set one field of one slot to `value`, and record it as an edit.
 *
 * **The editor sends numbers, never bytes.** Packing is the source's — tight
 * for a script or mesh, std140 for a shader's Material block — and it is
 * decided by the declaration the engine already parses. A client that packed
 * its own blob would be the second implementation of a layout rule, and the
 * first time a type's size changed the two would disagree silently and the
 * renderer would read garbage. So this takes a field index and up to four
 * components, rebuilds the whole block from the values already resolved, writes
 * the one field, and hands the bytes to the entity api.
 *
 * `slot` is an index into the same order bridge_params_collect reports. Returns
 * 1 on success, 0 when the entity, slot or field does not exist — which is not
 * an error: the editor racing the engine by one frame is ordinary, and a stale
 * field index is what that looks like here.
 */
int32_t bridge_params_write(const struct entity_api *entity,
			    const struct asset_api *asset, int32_t id,
			    int32_t slot, int32_t field, const float value[4]);

#endif /* BRIDGE_PARAMS_H */
