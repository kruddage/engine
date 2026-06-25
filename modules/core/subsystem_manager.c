/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "subsystem_manager.h"

#include <stdint.h>
#include <string.h>

void subsystem_manager_init(struct subsystem_manager *mgr,
			    const struct subsystem *table)
{
	int32_t i;

	mgr->static_table  = table;
	mgr->dynamic_count = 0;

	for (i = 0; table[i].name; i++) {
		if (table[i].init)
			table[i].init();
	}
}

void subsystem_manager_register(struct subsystem_manager *mgr,
				const struct subsystem *desc)
{
	int32_t slot;

	if (mgr->dynamic_count >= SUBSYSTEM_MANAGER_MAX_DYNAMIC)
		return;

	slot = mgr->dynamic_count++;
	mgr->dynamic[slot] = *desc;

	if (mgr->dynamic[slot].init)
		mgr->dynamic[slot].init();
}

const void *subsystem_manager_get_api(const struct subsystem_manager *mgr,
				      const char *name)
{
	int32_t i;

	for (i = 0; mgr->static_table[i].name; i++) {
		if (strcmp(mgr->static_table[i].name, name) == 0)
			return mgr->static_table[i].api;
	}

	for (i = 0; i < mgr->dynamic_count; i++) {
		if (strcmp(mgr->dynamic[i].name, name) == 0)
			return mgr->dynamic[i].api;
	}

	return NULL;
}

void subsystem_manager_tick(struct subsystem_manager *mgr)
{
	int32_t i;

	for (i = 0; mgr->static_table[i].name; i++) {
		if (mgr->static_table[i].tick)
			mgr->static_table[i].tick();
	}

	for (i = 0; i < mgr->dynamic_count; i++) {
		if (mgr->dynamic[i].tick)
			mgr->dynamic[i].tick();
	}
}

void subsystem_manager_shutdown(struct subsystem_manager *mgr)
{
	int32_t i;
	int32_t static_len;

	for (static_len = 0; mgr->static_table[static_len].name; static_len++)
		;

	for (i = mgr->dynamic_count - 1; i >= 0; i--) {
		if (mgr->dynamic[i].shutdown)
			mgr->dynamic[i].shutdown();
	}

	for (i = static_len - 1; i >= 0; i--) {
		if (mgr->static_table[i].shutdown)
			mgr->static_table[i].shutdown();
	}
}
