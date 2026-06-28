/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/fetch.h>
#include <GLES3/gl3.h>
#include <stdint.h>

/*
 * VAO entry points used by Dear ImGui's GLES backend (imgui_plugin.wasm).
 * ImGui calls the GL_OES_vertex_array_object-suffixed names; their Khronos
 * prototypes live in <GLES2/gl2ext.h> behind GL_GLEXT_PROTOTYPES, so declare
 * the three we need directly to keep the include surface small.
 */
extern void glGenVertexArraysOES(GLsizei n, GLuint *arrays);
extern void glBindVertexArrayOES(GLuint array);
extern void glDeleteVertexArraysOES(GLsizei n, const GLuint *arrays);

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

/*
 * IndexedDB project-asset persistence for backend_plugin.wasm.  IDB is
 * asynchronous and EM_JS bodies must live in the main module (a side
 * module is a bare .wasm with no JS output, so its EM_JS would become a
 * throwing stub).  The side module drives these as plain C->JS calls and
 * polls krudd_idb_state() via emscripten_async_call; it never relies on
 * JS calling back into the side module by name.  Loaded records are staged
 * in a JS-side queue and drained by krudd_idb_peek / krudd_idb_pop.
 *
 * Store:  database "krudd-project", object store "assets" (keyPath "id").
 * Record: { version:1, id:uint32, path:string, type:int32, data:ArrayBuffer }.
 */
EM_JS(void, krudd_idb_open, (void), {
	Module.__kruddIdb = { state: 0, queue: [] };
	var st = Module.__kruddIdb;
	try {
		var req = indexedDB.open("krudd-project", 1);
		req.onupgradeneeded = function(ev) {
			var db = ev.target.result;
			if (!db.objectStoreNames.contains("assets"))
				db.createObjectStore("assets", { keyPath: "id" });
		};
		req.onerror = function() { st.state = 2; };
		req.onsuccess = function(ev) {
			var db = ev.target.result;
			var tx = db.transaction("assets", "readonly");
			var cur = tx.objectStore("assets").openCursor();
			cur.onerror = function() { st.state = 1; };
			cur.onsuccess = function(cev) {
				var c = cev.target.result;
				if (!c) { st.state = 1; return; }
				var rec = c.value;
				/* Best-effort: skip records we don't understand. */
				if ((rec.version >>> 0) === 1)
					st.queue.push(rec);
				c.continue();
			};
		};
	} catch (e) {
		st.state = 2;
	}
})

/* 0 = loading, 1 = ready (queue populated), 2 = unavailable. */
EM_JS(int, krudd_idb_state, (void), {
	return Module.__kruddIdb ? Module.__kruddIdb.state : 2;
})

/*
 * Inspect the front staged record without removing it: writes id, type and
 * (NUL-terminated, truncated to path_cap) path, and returns the data byte
 * length, or -1 when the queue is empty.
 */
EM_JS(int, krudd_idb_peek, (uint32_t *id_out, int32_t *type_out,
			    char *path_out, uint32_t path_cap), {
	var st = Module.__kruddIdb;
	if (!st || st.queue.length === 0)
		return -1;
	var rec = st.queue[0];
	HEAPU32[id_out >> 2] = rec.id >>> 0;
	HEAP32[type_out >> 2] = rec.type | 0;
	stringToUTF8(rec.path || "", path_out, path_cap);
	var ab = rec.data;
	return ab ? ab.byteLength : 0;
})

/* Copy the front record's bytes to dst (sized via peek) and dequeue it. */
EM_JS(void, krudd_idb_pop, (uint8_t *dst), {
	var st = Module.__kruddIdb;
	if (!st || st.queue.length === 0)
		return;
	var rec = st.queue.shift();
	var ab = rec.data;
	if (dst && ab && ab.byteLength > 0)
		HEAPU8.set(new Uint8Array(ab), dst);
})

/* Insert-or-update one record (copies bytes into an owned ArrayBuffer). */
EM_JS(void, krudd_idb_put, (uint32_t id, const char *path_ptr, int32_t type,
			    const void *data_ptr, uint32_t size), {
	var path = UTF8ToString(path_ptr);
	var ab = new ArrayBuffer(size);
	if (size > 0)
		new Uint8Array(ab).set(HEAPU8.subarray(data_ptr, data_ptr + size));
	var rec = { version: 1, id: id >>> 0, path: path, type: type | 0,
		    data: ab };
	try {
		var req = indexedDB.open("krudd-project", 1);
		req.onupgradeneeded = function(ev) {
			var db = ev.target.result;
			if (!db.objectStoreNames.contains("assets"))
				db.createObjectStore("assets", { keyPath: "id" });
		};
		req.onsuccess = function(ev) {
			var db = ev.target.result;
			db.transaction("assets", "readwrite")
			  .objectStore("assets").put(rec);
		};
	} catch (e) {}
})

/* Remove the record with the given id. */
EM_JS(void, krudd_idb_del, (uint32_t id), {
	try {
		var req = indexedDB.open("krudd-project", 1);
		req.onupgradeneeded = function(ev) {
			var db = ev.target.result;
			if (!db.objectStoreNames.contains("assets"))
				db.createObjectStore("assets", { keyPath: "id" });
		};
		req.onsuccess = function(ev) {
			var db = ev.target.result;
			db.transaction("assets", "readwrite")
			  .objectStore("assets").delete(id >>> 0);
		};
	} catch (e) {}
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
	/* IndexedDB project-asset persistence — backend_plugin.wasm */
	(void *)krudd_idb_open,
	(void *)krudd_idb_state,
	(void *)krudd_idb_peek,
	(void *)krudd_idb_pop,
	(void *)krudd_idb_put,
	(void *)krudd_idb_del,
	/* Async polling for backend IDB readiness — backend_plugin.wasm */
	(void *)emscripten_async_call,
};

/*
 * WebGL / GLES drawing functions.  Same rationale as above: these are JS
 * library functions and are only added to asmLibraryArg when the main module's
 * own WASM imports them.  The main module renders nothing itself — the WebGL 2
 * calls all live in renderer_webgl.wasm and imgui_plugin.wasm (the latter via
 * Dear ImGui's GLES backend) — so without taking their addresses here they
 * would all be throwing stubs at runtime.  scripts/check-plugin-symbols.mjs
 * verifies that every gl* a plugin imports is covered by this list.
 */
EMSCRIPTEN_KEEPALIVE void *gl_abi_refs[] = {
	/* Buffers and vertex array objects */
	(void *)glGenBuffers,
	(void *)glBindBuffer,
	(void *)glBufferData,
	(void *)glBufferSubData,
	(void *)glDeleteBuffers,
	(void *)glGenVertexArraysOES,
	(void *)glBindVertexArrayOES,
	(void *)glDeleteVertexArraysOES,
	/* Shaders and programs */
	(void *)glCreateShader,
	(void *)glShaderSource,
	(void *)glCompileShader,
	(void *)glGetShaderiv,
	(void *)glGetShaderInfoLog,
	(void *)glDeleteShader,
	(void *)glAttachShader,
	(void *)glDetachShader,
	(void *)glCreateProgram,
	(void *)glLinkProgram,
	(void *)glGetProgramiv,
	(void *)glGetProgramInfoLog,
	(void *)glUseProgram,
	(void *)glDeleteProgram,
	(void *)glIsProgram,
	/* Attributes and uniforms */
	(void *)glGetAttribLocation,
	(void *)glGetUniformLocation,
	(void *)glUniform1i,
	(void *)glUniformMatrix4fv,
	(void *)glEnableVertexAttribArray,
	(void *)glVertexAttribPointer,
	/* Textures */
	(void *)glGenTextures,
	(void *)glBindTexture,
	(void *)glActiveTexture,
	(void *)glTexImage2D,
	(void *)glTexParameteri,
	(void *)glDeleteTextures,
	/* Pipeline state */
	(void *)glEnable,
	(void *)glDisable,
	(void *)glIsEnabled,
	(void *)glGetIntegerv,
	(void *)glBlendEquation,
	(void *)glBlendEquationSeparate,
	(void *)glBlendFuncSeparate,
	(void *)glScissor,
	(void *)glViewport,
	/* Framebuffer and clear */
	(void *)glBindFramebuffer,
	(void *)glClear,
	(void *)glClearColor,
	(void *)glClearDepthf,
	/* Draw */
	(void *)glDrawElements,
	(void *)glDrawElementsInstanced,
};
#endif /* __EMSCRIPTEN__ */
