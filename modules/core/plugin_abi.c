/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/fetch.h>

/*
 * Force the Emscripten linker to include the HTML5 and Fetch API JS library
 * functions in the main module.  The linker only includes a JS library
 * function in asmLibraryArg when the main module's own WASM imports it.
 * Side modules (plugins) that call these functions resolve their imports
 * from asmLibraryArg at runtime; if the function is absent there the
 * dynamic linker substitutes a stub that throws on invocation.
 *
 * None of the main module's own C code calls these functions — they are
 * used exclusively in plugin side modules — so without this file they
 * would all be stubs.
 */
EMSCRIPTEN_KEEPALIVE void *plugin_abi_refs[] = {
	/* HTML5 WebGL context — renderer_webgl.wasm */
	(void *)emscripten_webgl_init_context_attributes,
	(void *)emscripten_webgl_create_context,
	(void *)emscripten_webgl_make_context_current,
	(void *)emscripten_webgl_destroy_context,
	/* HTML5 input events — imgui_plugin.wasm, kruddboard.wasm */
	(void *)emscripten_set_mousemove_callback,
	(void *)emscripten_set_mousedown_callback,
	(void *)emscripten_set_mouseup_callback,
	(void *)emscripten_set_touchstart_callback,
	(void *)emscripten_set_touchmove_callback,
	(void *)emscripten_set_touchend_callback,
	(void *)emscripten_set_touchcancel_callback,
	(void *)emscripten_set_keydown_callback,
	/* HTML5 canvas dimensions — imgui_plugin.wasm */
	(void *)emscripten_get_element_css_size,
	(void *)emscripten_set_canvas_element_size,
	/* Fetch API — asset_plugin.wasm */
	(void *)emscripten_fetch,
	(void *)emscripten_fetch_close,
};
#endif /* __EMSCRIPTEN__ */
