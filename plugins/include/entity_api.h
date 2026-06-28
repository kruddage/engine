/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ENTITY_API_H
#define ENTITY_API_H

#include "world.h"

#include <stdint.h>

/*
 * Cross-plugin access to the live entity world, published as the "scene"
 * subsystem api:
 *
 *   const struct entity_api *e = subsystem_manager_get_api(mgr, "scene");
 *   const struct world      *w = e->get_world();
 *
 * A renderer walks w->count entities, skips tombstones (alive[i] == 0), tests
 * each mask for COMPONENT_RENDER, and draws using world_xform[i] together with
 * render_ref[i].
 */
struct entity_api {
	const struct world *(*get_world)(void);
	/* Load and ingest a .scene asset by path; 0 on success, -1 otherwise. */
	int32_t             (*load_scene)(const char *path);
};

#endif /* ENTITY_API_H */
