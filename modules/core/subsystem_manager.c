/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "subsystem_manager.h"

#include <stdint.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define PLUGIN_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define PLUGIN_EXPORT
#endif

void subsystem_manager_init(struct subsystem_manager *mgr,
			    const struct subsystem *table)
{
	int32_t i;

	mgr->static_table        = table;
	mgr->dynamic_count       = 0;
	mgr->async_dynamic_count = 0;

	for (i = 0; table[i].name; i++) {
		if (table[i].init)
			table[i].init();
	}
}

PLUGIN_EXPORT void subsystem_manager_register(struct subsystem_manager *mgr,
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

static void async_done(void *raw)
{
	struct async_subsystem_slot *s = raw;
	int32_t i;

	s->ready = 1;
	for (i = 0; i < s->cb_count; i++)
		s->cbs[i].fn(s->cbs[i].ctx);
	s->cb_count = 0;
}

PLUGIN_EXPORT void
subsystem_manager_register_async(struct subsystem_manager *mgr,
				 const struct async_subsystem *desc)
{
	struct async_subsystem_slot *s;
	int32_t slot;

	if (mgr->async_dynamic_count >= SUBSYSTEM_MANAGER_MAX_ASYNC)
		return;

	slot = mgr->async_dynamic_count++;
	s = &mgr->async_dynamic[slot];
	s->desc     = *desc;
	s->ready    = 0;
	s->cb_count = 0;

	if (desc->async_init)
		desc->async_init(async_done, s);
}

PLUGIN_EXPORT void subsystem_manager_on_ready(struct subsystem_manager *mgr,
					      const char *name,
					      void (*cb)(void *ctx),
					      void *ctx)
{
	struct async_subsystem_slot *s;
	int32_t i;

	for (i = 0; i < mgr->async_dynamic_count; i++) {
		s = &mgr->async_dynamic[i];
		if (strcmp(s->desc.name, name) != 0)
			continue;
		if (s->ready) {
			cb(ctx);
			return;
		}
		if (s->cb_count < SUBSYSTEM_MANAGER_MAX_READY_CBS)
			s->cbs[s->cb_count++] =
				(struct async_ready_cb){ cb, ctx };
		return;
	}
}

PLUGIN_EXPORT const void *subsystem_manager_get_api(const struct subsystem_manager *mgr,
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

	for (i = 0; i < mgr->async_dynamic_count; i++) {
		if (strcmp(mgr->async_dynamic[i].desc.name, name) == 0)
			return mgr->async_dynamic[i].desc.api;
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

	for (i = 0; i < mgr->async_dynamic_count; i++) {
		if (mgr->async_dynamic[i].ready && mgr->async_dynamic[i].desc.tick)
			mgr->async_dynamic[i].desc.tick();
	}
}

void subsystem_manager_shutdown(struct subsystem_manager *mgr)
{
	int32_t i;
	int32_t static_len;

	for (static_len = 0; mgr->static_table[static_len].name; static_len++)
		;

	for (i = mgr->async_dynamic_count - 1; i >= 0; i--) {
		if (mgr->async_dynamic[i].ready &&
		    mgr->async_dynamic[i].desc.shutdown)
			mgr->async_dynamic[i].desc.shutdown();
	}

	for (i = mgr->dynamic_count - 1; i >= 0; i--) {
		if (mgr->dynamic[i].shutdown)
			mgr->dynamic[i].shutdown();
	}

	for (i = static_len - 1; i >= 0; i--) {
		if (mgr->static_table[i].shutdown)
			mgr->static_table[i].shutdown();
	}
}
