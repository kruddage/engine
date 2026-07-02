/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The WASM heap grows dynamically (-sALLOW_MEMORY_GROWTH=1). Current engines
 * back a growable WebAssembly.Memory with a resizable ArrayBuffer, and the
 * WebCrypto spec forbids crypto.getRandomValues() on a view over one. That
 * trips inside Emscripten's own runtime startup (_random_get -> randomFill
 * -> initRandomFill), which seeds the C runtime's RNG from a HEAPU8 view
 * before main() ever runs -- so it is fatal on affected browsers.
 *
 * Route resizable-buffer views through a fixed-size scratch buffer instead;
 * behaviour is identical, it just fills the caller's view via a copy rather
 * than in place.
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
