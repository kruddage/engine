/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ENTITY_SCRIPT_H
#define ENTITY_SCRIPT_H

#include <stdint.h>

/*
 * entity_script — the bridge between the world's COMPONENT_SCRIPT entities and
 * the S7 image's entity-script dispatcher (core/entity_script.scm).
 *
 * It registers the entity-* host primitives a script clause calls, then each
 * frame walks every scripted entity, fetches the bytes of its bound
 * ASSET_TYPE_SCRIPT asset, and hands (id, source, clock) to entity-script-tick.
 *
 * Reads target the authored rest pose (world.local[]); writes target the
 * animated render pose (world.world_xform[]). Because propagation refills
 * world_xform from local at the top of every world_tick, a script must run
 * AFTER propagation and its writes last only for the frame — the authored
 * transform is never clobbered and time-based animation cannot drift.
 */

struct world;
struct asset_api;

/* Register the entity-* primitives against the shared interpreter. Idempotent;
 * a no-op if the interpreter is unavailable. */
void entity_script_init(void);

/*
 * Drive one frame of scripts. For each live entity carrying COMPONENT_SCRIPT,
 * resolve its script_ref to source bytes via `asset` and call the image's
 * entity-script-tick with the clock `t` (seconds). A NULL world or asset api,
 * or a down interpreter, makes this a no-op.
 */
void entity_script_tick(struct world *w, const struct asset_api *asset,
			float t);

#endif /* ENTITY_SCRIPT_H */
