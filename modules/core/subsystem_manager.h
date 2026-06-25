/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SUBSYSTEM_MANAGER_H
#define SUBSYSTEM_MANAGER_H

#include "subsystem.h"
#include "async_subsystem.h"
#include <stdint.h>

#define SUBSYSTEM_MANAGER_MAX_DYNAMIC    16
#define SUBSYSTEM_MANAGER_MAX_ASYNC       8
#define SUBSYSTEM_MANAGER_MAX_READY_CBS   8

struct async_ready_cb {
	void (*fn)(void *ctx);
	void *ctx;
};

struct async_subsystem_slot {
	struct async_subsystem  desc;
	int32_t                 ready;
	int32_t                 cb_count;
	struct async_ready_cb   cbs[SUBSYSTEM_MANAGER_MAX_READY_CBS];
};

/*
 * Holds the static compile-time subsystem table plus any subsystems
 * registered at runtime by plugins. Both are driven together by the
 * manager's tick and shutdown calls.
 */
struct subsystem_manager {
	const struct subsystem     *static_table;
	struct subsystem            dynamic[SUBSYSTEM_MANAGER_MAX_DYNAMIC];
	int32_t                     dynamic_count;
	struct async_subsystem_slot async_dynamic[SUBSYSTEM_MANAGER_MAX_ASYNC];
	int32_t                     async_dynamic_count;
};

/* Init the manager and call init on every static subsystem. */
void subsystem_manager_init(struct subsystem_manager *mgr,
			    const struct subsystem *table);

/*
 * Register a dynamically loaded subsystem (called from plugin_entry).
 * Immediately calls its init so the plugin is live after registration.
 */
void subsystem_manager_register(struct subsystem_manager *mgr,
				const struct subsystem *desc);

/*
 * Register an async subsystem. Calls async_init immediately; the
 * subsystem signals completion by invoking the done callback the
 * manager passes in. tick runs only once ready is signalled.
 */
void subsystem_manager_register_async(struct subsystem_manager *mgr,
				      const struct async_subsystem *desc);

/*
 * Register cb to be called when the named async subsystem is ready.
 * If it is already ready, cb is called immediately before returning.
 */
void subsystem_manager_on_ready(struct subsystem_manager *mgr,
				const char *name,
				void (*cb)(void *ctx),
				void *ctx);

/*
 * Look up the api pointer registered by the named subsystem.
 * Returns NULL if no subsystem with that name is found.
 */
const void *subsystem_manager_get_api(const struct subsystem_manager *mgr,
				      const char *name);

void subsystem_manager_tick(struct subsystem_manager *mgr);

/* Shuts down dynamic subsystems first, then static, both in reverse order. */
void subsystem_manager_shutdown(struct subsystem_manager *mgr);

#endif /* SUBSYSTEM_MANAGER_H */
