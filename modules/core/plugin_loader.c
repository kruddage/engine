/* SPDX-License-Identifier: MIT */
#include "plugin_loader.h"
#include "log.h"

#include <stddef.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <dlfcn.h>

static void (*plugin_tick_fn)(void);
static void (*plugin_shutdown_fn)(void);

static void on_load(void *user_data, void *handle)
{
	void (*init)(void);
	void (*tick)(void);
	void (*shutdown)(void);

	*(void **)(&init)     = dlsym(handle, "hello_init");
	*(void **)(&tick)     = dlsym(handle, "hello_tick");
	*(void **)(&shutdown) = dlsym(handle, "hello_shutdown");

	LOG_INFO("plugin_loader: hello_plugin loaded");
	plugin_tick_fn     = tick;
	plugin_shutdown_fn = shutdown;
	if (init)
		init();
}

static void on_error(void *user_data)
{
	LOG_INFO("plugin_loader: failed to load hello_plugin.wasm");
}
#endif /* __EMSCRIPTEN__ */

void plugin_loader_init(void)
{
#ifdef __EMSCRIPTEN__
	emscripten_dlopen("hello_plugin.wasm", RTLD_NOW, NULL,
	                  on_load, on_error);
#endif
}

void plugin_loader_tick(void)
{
#ifdef __EMSCRIPTEN__
	if (plugin_tick_fn)
		plugin_tick_fn();
#endif
}

void plugin_loader_shutdown(void)
{
#ifdef __EMSCRIPTEN__
	if (plugin_shutdown_fn)
		plugin_shutdown_fn();
#endif
}
