/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "plugin_loader.h"
#include "log.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <dlfcn.h>

static struct subsystem_manager  *manager;
static const char * const        *plugin_list;
static int32_t                    load_index;

/*
 * Query the Performance Resource Timing API for the decoded byte size of
 * a fetched WASM plugin.  The file may have been renamed with a commit
 * hash by locateFile, so we match by base name (sans the .wasm suffix).
 *
 * EM_JS is used rather than EM_ASM_INT because EM_ASM_INT stringifies
 * its code argument as a named macro parameter: the C preprocessor
 * tokenises the body before stringification, so $N arguments trigger
 * -Wdollar-in-identifier-extension.  EM_JS puts the body in __VA_ARGS__,
 * so commas are captured and re-emitted by #__VA_ARGS__, and C parameter
 * names replace $N entirely.
 */
EM_JS(int32_t, plugin_loader_wasm_size, (const char *name_c), {
	var name = UTF8ToString(name_c);
	var base = name.slice(0, name.length - 5);
	var entries = performance.getEntriesByType("resource");
	for (var i = entries.length - 1; i >= 0; i--) {
		var e = entries[i];
		if (e.name.indexOf(base) !== -1 &&
		    e.name.slice(-5) === ".wasm") {
			var sz = e.decodedBodySize || e.encodedBodySize || 0;
			if (sz > 0) return sz;
		}
	}
	return 0;
})

static void load_next(void);

static void on_load(void *user_data, void *handle)
{
	void (*entry)(struct subsystem_manager *);
	int32_t prev_count;
	int32_t wasm_size;
	int32_t i;

	*(void **)(&entry) = dlsym(handle, "plugin_entry");
	if (!entry) {
		LOG_INFO("plugin_loader: no plugin_entry in %s",
			 (const char *)user_data);
	} else {
		LOG_INFO("plugin_loader: loaded %s", (const char *)user_data);
		prev_count = manager->dynamic_count;
		entry(manager);
		wasm_size = plugin_loader_wasm_size((const char *)user_data);
		for (i = prev_count; i < manager->dynamic_count; i++)
			manager->dynamic[i].wasm_size = (uint32_t)wasm_size;
	}

	load_next();
}

static void on_error(void *user_data)
{
	LOG_INFO("plugin_loader: failed to load %s", (const char *)user_data);
	load_next();
}

/*
 * Load plugins strictly one at a time, in plugin_list order.  emscripten_dlopen
 * is asynchronous, so firing every plugin at once races: a plugin's WASM imports
 * are bound at instantiation, so a dependent (e.g. kruddboard, which calls into
 * imgui_plugin's ImGui symbols) loaded before its dependency binds those imports
 * to stubs that throw on first call.  Chaining each load off the previous one's
 * completion guarantees a plugin's dependencies are already in the global symbol
 * table — hence RTLD_GLOBAL — before it loads.  plugin_list must therefore be in
 * dependency order; scripts/check-plugin-symbols.mjs verifies that it is.
 */
static void load_next(void)
{
	const char *name;

	if (!plugin_list)
		return;
	name = plugin_list[load_index];
	if (!name)
		return; /* all plugins processed */
	load_index++;
	emscripten_dlopen(name, RTLD_NOW | RTLD_GLOBAL, (void *)name,
			  on_load, on_error);
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
	load_index = 0;
	load_next();
#else
	/* emscripten_dlopen unavailable on native; nothing to init. */
#endif
}

void plugin_loader_shutdown(void)
{
}
