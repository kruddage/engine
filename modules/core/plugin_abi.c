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

/*
 * Web text-input bridge for Dear ImGui — desktop keyboard + mobile soft
 * keyboard support.
 *
 * A hidden <input type="text"> element is used as the keyboard capture
 * target.  On desktop it receives key events; on mobile, focusing it
 * raises the soft keyboard.  imgui_plugin.wasm drives the lifecycle
 * via the five functions below.
 *
 * All five EM_JS bodies are defined here (main module) so they land in
 * asmLibraryArg and are reachable by imgui_plugin.wasm at runtime.
 * See plugin_abi.c header comment for why side modules must not define
 * EM_JS themselves.
 */

/*
 * krudd_text_input_init — call once at imgui_plugin startup.
 *
 * Creates the hidden <input> element, attaches it to document.body,
 * and wires up the event listeners that populate the pending char
 * string and key-code queue consumed by drain/pop below.
 *
 * Key-code mapping (kept in sync with imgui_plugin.cpp):
 *   Backspace=1  Enter=2    Tab=3       Delete=4
 *   ArrowLeft=5  ArrowRight=6  ArrowUp=7  ArrowDown=8
 *   Home=9       End=10     Escape=11
 *
 * Printable characters arrive through the 'input' event; navigation /
 * editing keys arrive through 'keydown' and are preventDefault()ed so
 * the browser does not act on them (e.g. Tab navigating focus away).
 */
EM_JS(void, krudd_text_input_init, (void), {
	if (Module.__kruddText)
		return;
	Module.__kruddText = { chars: "", keys: [] };

	var el = document.createElement("input");
	el.type           = "text";
	el.autocapitalize = "off";
	el.autocomplete   = "off";
	el.autocorrect    = "off";
	el.spellcheck     = false;
	/* Position offscreen so it never flashes or shifts layout */
	el.style.position = "absolute";
	el.style.left     = "-9999px";
	el.style.top      = "-9999px";
	el.style.width    = "1px";
	el.style.height   = "1px";
	el.style.opacity  = "0";
	document.body.appendChild(el);
	Module.__kruddTextEl = el;

	/*
	 * Printable / IME text.  A composing keyboard (predictive text,
	 * many mobile IMEs) fires "input" both mid-composition and again on
	 * commit; capturing both double-types every character.  Skip events
	 * while ev.isComposing and take the committed text on
	 * compositionend instead, so each character is captured exactly once.
	 */
	el.addEventListener("input", function(ev) {
		if (ev.isComposing)
			return;
		Module.__kruddText.chars += el.value;
		el.value = "";
	});
	el.addEventListener("compositionend", function() {
		Module.__kruddText.chars += el.value;
		el.value = "";
	});

	/* Navigation / editing keys: push a small integer code */
	var KEY_CODES = {
		"Backspace":  1,
		"Enter":      2,
		"Tab":        3,
		"Delete":     4,
		"ArrowLeft":  5,
		"ArrowRight": 6,
		"ArrowUp":    7,
		"ArrowDown":  8,
		"Home":       9,
		"End":        10,
		"Escape":     11
	};
	el.addEventListener("keydown", function(ev) {
		var code = KEY_CODES[ev.key];
		if (code) {
			Module.__kruddText.keys.push(code);
			ev.preventDefault();
		}
	});
})

/*
 * krudd_text_input_show — focus the hidden input element.
 *
 * On desktop this begins capturing keyboard events.  On mobile this
 * raises the soft keyboard.  Must be called from within a user-gesture
 * handler on iOS (see imgui_plugin.cpp on_touch for the caveat).
 */
EM_JS(void, krudd_text_input_show, (void), {
	if (Module.__kruddTextEl)
		Module.__kruddTextEl.focus();
})

/*
 * krudd_text_input_hide — blur the hidden input element.
 *
 * On mobile this dismisses the soft keyboard.
 */
EM_JS(void, krudd_text_input_hide, (void), {
	if (Module.__kruddTextEl)
		Module.__kruddTextEl.blur();
})

/*
 * krudd_text_input_drain_chars — copy pending UTF-8 text into buf.
 *
 * Copies at most cap-1 bytes, NUL-terminates, clears the pending
 * string, and returns the byte count written (excluding NUL).
 * Returns 0 if nothing is pending.
 */
EM_JS(int, krudd_text_input_drain_chars, (char *buf, int cap), {
	if (!Module.__kruddText || !Module.__kruddText.chars)
		return 0;
	var str   = Module.__kruddText.chars;
	var bytes = lengthBytesUTF8(str);
	/* Truncate to fit within cap-1 bytes (cap includes the NUL) */
	if (bytes >= cap) {
		var truncated = "";
		var used = 0;
		for (var i = 0; i < str.length; i++) {
			var cp   = str.codePointAt(i);
			var clen = cp > 0x7FF ? (cp > 0xFFFF ? 4 : 3)
			                      : (cp > 0x7F   ? 2 : 1);
			if (used + clen >= cap)
				break;
			truncated += str[i];
			if (cp > 0xFFFF)
				i++; /* surrogate pair: skip low surrogate */
			used += clen;
		}
		str = truncated;
	}
	stringToUTF8(str, buf, cap);
	Module.__kruddText.chars = "";
	return lengthBytesUTF8(str);
})

/*
 * krudd_text_input_pop_key — dequeue one key code.
 *
 * Returns the oldest queued key code (1-11) or 0 if the queue is
 * empty.  The caller loops until 0 is returned.
 */
EM_JS(int, krudd_text_input_pop_key, (void), {
	if (!Module.__kruddText || !Module.__kruddText.keys.length)
		return 0;
	return Module.__kruddText.keys.shift();
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
	/* Web text-input bridge — imgui_plugin.wasm */
	(void *)krudd_text_input_init,
	(void *)krudd_text_input_show,
	(void *)krudd_text_input_hide,
	(void *)krudd_text_input_drain_chars,
	(void *)krudd_text_input_pop_key,
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
	/* Core GLES3 VAO + UBO entry points — renderer_webgl.wasm */
	(void *)glGenVertexArrays,
	(void *)glBindVertexArray,
	(void *)glDeleteVertexArrays,
	(void *)glBindBufferRange,
	(void *)glGetUniformBlockIndex,
	(void *)glUniformBlockBinding,
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
