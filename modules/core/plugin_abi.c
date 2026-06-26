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
 *
 * The emscripten_set_*_callback names in html5.h are function-like macros
 * that expand to the _on_thread variants; reference those directly so that
 * the compiler sees a proper function identifier rather than an unexpanded
 * macro.
 *
 * EM_JS functions below are defined here (main module) so they are included
 * in asmLibraryArg and reachable by side modules via the dynamic linker.
 * Side modules must NOT use EM_ASM_* — inline JS strings are registered in
 * the main module's ASM_CONSTS table at link time, so a side module's inline
 * JS is never registered and emscripten_asm_const_* becomes a throwing stub.
 */

/*
 * Returns window.devicePixelRatio for high-DPI canvas scaling.
 * Called by imgui_plugin.wasm every tick; defined here so the JS function
 * lands in asmLibraryArg and is reachable by the side module.
 */
EM_JS(double, get_device_pixel_ratio, (void), {
	return window.devicePixelRatio || 1.0;
})

EMSCRIPTEN_KEEPALIVE void *plugin_abi_refs[] = {
	/* HTML5 WebGL context — renderer_webgl.wasm */
	(void *)emscripten_webgl_init_context_attributes,
	(void *)emscripten_webgl_create_context,
	(void *)emscripten_webgl_make_context_current,
	(void *)emscripten_webgl_destroy_context,
	/* HTML5 input events — imgui_plugin.wasm, kruddboard.wasm.
	 * The emscripten_set_*_callback names are function-like macros;
	 * use the _on_thread functions they expand to. */
	(void *)emscripten_set_mousemove_callback_on_thread,
	(void *)emscripten_set_mousedown_callback_on_thread,
	(void *)emscripten_set_mouseup_callback_on_thread,
	(void *)emscripten_set_touchstart_callback_on_thread,
	(void *)emscripten_set_touchmove_callback_on_thread,
	(void *)emscripten_set_touchend_callback_on_thread,
	(void *)emscripten_set_touchcancel_callback_on_thread,
	(void *)emscripten_set_keydown_callback_on_thread,
	/* HTML5 canvas dimensions — imgui_plugin.wasm */
	(void *)emscripten_get_element_css_size,
	(void *)emscripten_set_canvas_element_size,
	/* Device pixel ratio — imgui_plugin.wasm */
	(void *)get_device_pixel_ratio,
	/* Fetch API — asset_plugin.wasm */
	(void *)emscripten_fetch,
	(void *)emscripten_fetch_close,
};
#endif /* __EMSCRIPTEN__ */
