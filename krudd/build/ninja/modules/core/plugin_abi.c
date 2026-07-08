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
 * Emscripten JS-library glue kept in the main module.  Every plugin is now
 * compiled straight into this single WASM module (there are no side modules),
 * so a plugin's own reference to an HTML5 / Fetch / WebGL JS-library function
 * is what pulls that function into asmLibraryArg.  Two things still live here:
 *
 *   - The EM_JS bridges below (get_device_pixel_ratio and the text-input
 *     helpers).  An EM_JS body must be defined in the module that links the JS
 *     glue, and the imgui plugin calls these.
 *
 *   - The address-taking arrays further down.  These predate the single-module
 *     collapse, when they forced JS-library symbols into asmLibraryArg so side
 *     modules could resolve them at runtime.  In one static module they are
 *     most likely redundant, but confirming that needs a browser run (a missing
 *     symbol becomes a throwing stub, not a link error), so they are retained
 *     until that verification happens.
 *
 * The emscripten_set_*_callback names in html5.h are function-like macros that
 * expand to the _on_thread variants; reference those directly so the compiler
 * sees a proper function identifier rather than an unexpanded macro.
 */

/*
 * Returns window.devicePixelRatio for high-DPI canvas scaling.
 * Called by the imgui plugin every tick; defined here so the JS function
 * lands in asmLibraryArg and is reachable from the plugin's code.
 */
EM_JS(double, get_device_pixel_ratio, (void), {
	return window.devicePixelRatio || 1.0;
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
 * asmLibraryArg; the imgui plugin, compiled into the same module, calls them.
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
	/* chars: pending UTF-8 to feed ImGui.  keys: pending key codes.
	 * emitted: code units of the active composition already pushed to
	 * chars, so we never double-type (see emitSuffix). */
	Module.__kruddText = { chars: "", keys: [], emitted: 0 };

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
	 * Printable / IME text.
	 *
	 * A composing keyboard (predictive text, gesture typing, many mobile
	 * IMEs — notably Firefox Android) holds the current word in an open
	 * composition, firing "input" with ev.isComposing === true for each
	 * keystroke and only committing on space/enter.  The old code skipped
	 * every composing event and waited for compositionend, so nothing
	 * appeared until the word was committed ("typing doesn't show up until
	 * I press Enter").
	 *
	 * Instead, emit the *suffix* of el.value not yet pushed to ImGui, on
	 * every input event including composing ones, tracking how much we've
	 * emitted so a character is never sent twice.  This shows text live as
	 * it is typed while still de-duping the mid-composition + commit double
	 * fire.  During composition we must NOT clear el.value (that would
	 * break the IME), so we only clear once the text is committed.
	 *
	 * Limitation: predictive *replacement* of the composed word (e.g.
	 * autocorrect rewriting "teh"->"the") can't be un-emitted and would
	 * leave the earlier spelling.  The element disables autocapitalize,
	 * autocorrect and spellcheck above precisely to keep input append-only.
	 */
	function emitSuffix() {
		var v = el.value;
		var kt = Module.__kruddText;
		if (kt.emitted > v.length)	/* shrank: backspace / rewrite */
			kt.emitted = v.length;
		if (v.length > kt.emitted) {
			kt.chars += v.slice(kt.emitted);
			kt.emitted = v.length;
		}
	}

	el.addEventListener("compositionstart", function() {
		Module.__kruddText.emitted = 0;
	});
	el.addEventListener("input", function(ev) {
		emitSuffix();
		if (ev.isComposing)
			return;		/* keep composing; clearing breaks IME */
		el.value = "";
		Module.__kruddText.emitted = 0;
	});
	el.addEventListener("compositionend", function() {
		emitSuffix();		/* any tail not yet live-emitted */
		el.value = "";
		Module.__kruddText.emitted = 0;
	});
	/* Flush any uncommitted composition if focus is torn away (e.g. our
	 * own hide()), so in-progress text is not lost. */
	el.addEventListener("blur", function() {
		emitSuffix();
		el.value = "";
		Module.__kruddText.emitted = 0;
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
	(void *)emscripten_webgl_get_drawing_buffer_size,
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
	(void *)glDepthFunc,
	(void *)glDepthMask,
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
