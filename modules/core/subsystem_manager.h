/* SPDX-License-Identifier: MIT */
#ifndef SUBSYSTEM_MANAGER_H
#define SUBSYSTEM_MANAGER_H

#include "subsystem.h"
#include <stdint.h>

#define SUBSYSTEM_MANAGER_MAX_DYNAMIC 16

/*
 * Holds the static compile-time subsystem table plus any subsystems
 * registered at runtime by plugins. Both are driven together by the
 * manager's tick and shutdown calls.
 */
struct subsystem_manager {
	const struct subsystem *static_table;
	struct subsystem        dynamic[SUBSYSTEM_MANAGER_MAX_DYNAMIC];
	int32_t                 dynamic_count;
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

void subsystem_manager_tick(struct subsystem_manager *mgr);

/* Shuts down dynamic subsystems first, then static, both in reverse order. */
void subsystem_manager_shutdown(struct subsystem_manager *mgr);

#endif /* SUBSYSTEM_MANAGER_H */
