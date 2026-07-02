/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The WASM heap grows dynamically (-sALLOW_MEMORY_GROWTH=1). Current engines
 * back a growable WebAssembly.Memory with a resizable ArrayBuffer, and a
 * growing set of web APIs -- per spec -- reject a view over one:
 *
 *   - crypto.getRandomValues(): hit inside Emscripten's own runtime startup
 *     (_random_get -> randomFill -> initRandomFill), seeding the C runtime's
 *     RNG from a HEAPU8 view before main() ever runs -- fatal on load.
 *   - TextDecoder.prototype.decode(): hit on essentially every C string
 *     Emscripten marshals out of WASM memory (UTF8ArrayToString is the
 *     shared helper behind syscalls, paths, log messages, ...), so it can
 *     trip at any point during normal operation, not just startup.
 *
 * Wrap both to copy a resizable-buffer argument into a plain (non-resizable)
 * buffer before handing it to the native implementation. Reads (decode) copy
 * the input once; writes (getRandomValues) fill a scratch buffer and copy
 * the result back into the caller's view. Non-resizable buffers -- the
 * overwhelming common case once the heap stops growing -- pass straight
 * through with no copy.
 */
if (typeof crypto === 'object' && typeof crypto.getRandomValues === 'function') {
	var _krudd_getRandomValues = crypto.getRandomValues.bind(crypto);

	crypto.getRandomValues = function (view) {
		var buf = view && view.buffer;

		if (buf && buf.resizable) {
			var scratch = new Uint8Array(view.byteLength);

			_krudd_getRandomValues(scratch);
			new Uint8Array(view.buffer, view.byteOffset, view.byteLength)
				.set(scratch);
			return view;
		}
		return _krudd_getRandomValues(view);
	};
}

if (typeof TextDecoder === 'function') {
	var _krudd_decode = TextDecoder.prototype.decode;

	TextDecoder.prototype.decode = function (input, options) {
		if (input && input.buffer && input.buffer.resizable) {
			input = new Uint8Array(input.buffer, input.byteOffset,
				input.byteLength).slice();
		} else if (typeof ArrayBuffer !== 'undefined' &&
				input instanceof ArrayBuffer && input.resizable) {
			input = input.slice(0);
		}
		return _krudd_decode.call(this, input, options);
	};
}
