/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifdef __EMSCRIPTEN__
#include <emscripten.h>

/*
 * Emscripten JS-library glue kept in the main module.  Every plugin is compiled
 * straight into this single WASM module (there are no side modules and no
 * dynamic loading), so a plugin's own reference to an HTML5 / Fetch / WebGL
 * JS-library function is what pulls that function into asmLibraryArg — no
 * address-taking table is needed to force those symbols in (an earlier version
 * kept plugin_abi_refs[] / gl_abi_refs[] for exactly that, a leftover of the
 * side-module era, now removed).
 *
 * What must still live here are the EM_JS bridges below (get_device_pixel_ratio
 * and the text-input helpers): an EM_JS body has to be defined in the module
 * that links the JS glue, and the imgui plugin — compiled into this same module
 * — calls them.
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
 * kruddgui text-capture handoff.
 *
 * The keyboard bridge below (the hidden <input> and its char / key queues) is
 * one shared resource, drained each frame by whoever owns text input.  ImGui
 * owns it by default; when a kruddgui field takes focus, kruddgui raises this
 * flag so imgui_plugin steps aside — skipping its char / key drain and its
 * desktop WantTextInput auto-focus — and kruddgui drains the bridge into its
 * own focused field instead.  This is the text-input twin of the pointer-input
 * inversion (#489): kruddgui owns input, ImGui is downstream.
 *
 * Plain C, not EM_JS: both plugins are compiled into this module and reach it
 * through the global symbol table, the same way they reach the EM_JS bridges.
 */
static int g_kgui_text_capture;

void krudd_text_input_set_capture(int on)
{
	g_kgui_text_capture = on ? 1 : 0;
}

int krudd_text_input_capture(void)
{
	return g_kgui_text_capture;
}

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

/*
 * krudd_is_touch_device — true if the browser reports touch support.
 *
 * Lets imgui_plugin and kruddboard tell mobile browsers apart from desktop:
 * on a touch device the soft keyboard is driven by an explicit on-screen
 * toggle instead of the WantTextInput-driven auto-focus used on desktop
 * (see imgui_plugin.cpp), since auto-focusing the hidden <input> there pops
 * the native keyboard on every tap into the debug UI.
 */
EM_JS(int, krudd_is_touch_device, (void), {
	return (('ontouchstart' in window) || navigator.maxTouchPoints > 0) ? 1 : 0;
})
#endif /* __EMSCRIPTEN__ */
