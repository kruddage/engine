/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "plugin_loader.h"
#include "log.h"

#include <stddef.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <dlfcn.h>

static struct subsystem_manager *manager;

static void on_load(void *user_data, void *handle)
{
	void (*entry)(struct subsystem_manager *);

	*(void **)(&entry) = dlsym(handle, "plugin_entry");
	if (!entry) {
		LOG_INFO("plugin_loader: no plugin_entry symbol");
		return;
	}
	LOG_INFO("plugin_loader: calling plugin_entry");
	entry(manager);
}

static void on_error(void *user_data)
{
	LOG_INFO("plugin_loader: failed to load hello_plugin.wasm");
}
#endif /* __EMSCRIPTEN__ */

void plugin_loader_set_manager(struct subsystem_manager *mgr)
{
#ifdef __EMSCRIPTEN__
	manager = mgr;
#else
	(void)mgr;
#endif
}

void plugin_loader_init(void)
{
#ifdef __EMSCRIPTEN__
	emscripten_dlopen("hello_plugin.wasm", RTLD_NOW, NULL,
			  on_load, on_error);
#endif
}

void plugin_loader_shutdown(void)
{
}
