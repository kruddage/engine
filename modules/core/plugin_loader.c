/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "plugin_loader.h"
#include "log.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <dlfcn.h>

static struct subsystem_manager  *manager;
static const char * const        *plugin_list;

static void on_load(void *user_data, void *handle)
{
	void (*entry)(struct subsystem_manager *);

	*(void **)(&entry) = dlsym(handle, "plugin_entry");
	if (!entry) {
		LOG_INFO("plugin_loader: no plugin_entry in %s",
			 (const char *)user_data);
		return;
	}
	LOG_INFO("plugin_loader: loaded %s", (const char *)user_data);
	entry(manager);
}

static void on_error(void *user_data)
{
	LOG_INFO("plugin_loader: failed to load %s", (const char *)user_data);
}
#endif /* __EMSCRIPTEN__ */

void plugin_loader_set_manager(struct subsystem_manager *mgr)
{
#ifdef __EMSCRIPTEN__
	manager = mgr;
#else
	/* emscripten_dlopen unavailable on native; plugin loading unsupported. */
	(void)mgr;
#endif
}

void plugin_loader_set_plugins(const char * const *plugins)
{
#ifdef __EMSCRIPTEN__
	plugin_list = plugins;
#else
	/* emscripten_dlopen unavailable on native; plugin loading unsupported. */
	(void)plugins;
#endif
}

void plugin_loader_init(void)
{
#ifdef __EMSCRIPTEN__
	int32_t i;

	if (!plugin_list)
		return;
	for (i = 0; plugin_list[i]; i++) {
		emscripten_dlopen(plugin_list[i], RTLD_NOW,
				  (void *)plugin_list[i],
				  on_load, on_error);
	}
#else
	/* emscripten_dlopen unavailable on native; nothing to init. */
#endif
}

void plugin_loader_shutdown(void)
{
}
