/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "plugin_loader.h"

#include <stdio.h>

/*
 * Native smoke test: plugin_loader stubs are all intentional no-ops because
 * emscripten_dlopen is unavailable outside WASM.  Verify they don't crash.
 */
int main(void)
{
	plugin_loader_set_manager(NULL);
	plugin_loader_set_plugins(NULL);
	plugin_loader_init();
	plugin_loader_shutdown();

	printf("plugin_loader tests passed\n");
	return 0;
}
