/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * -sALLOW_MEMORY_GROWTH=1 makes WebAssembly.Memory's buffer a resizable
 * ArrayBuffer once memory grows. The Web Crypto spec forbids passing
 * getRandomValues() a view over a resizable buffer, which otherwise
 * breaks Emscripten's random_get() (libc PRNG seeding) on startup.
 */
if (typeof crypto !== 'undefined' && crypto.getRandomValues) {
	var nativeGetRandomValues = crypto.getRandomValues.bind(crypto);
	crypto.getRandomValues = function (view) {
		if (!view.buffer.resizable) {
			return nativeGetRandomValues(view);
		}
		var copy = new Uint8Array(view.byteLength);
		nativeGetRandomValues(copy);
		new Uint8Array(view.buffer, view.byteOffset, view.byteLength).set(copy);
		return view;
	};
}

window.addEventListener('error', function (e) {
	showError('Uncaught error', e.error ? (e.error.stack || e.message) : e.message);
});
window.addEventListener('unhandledrejection', function (e) {
	var reason = e.reason;
	var detail;
	if (reason == null || typeof reason !== 'object') {
		detail = String(reason);
	} else {
		detail = '';
		if (reason.message) detail += reason.message + '\n';
		if (reason.stack)   detail += reason.stack;
		if (!detail)        detail = String(reason);
	}
	showError('Unhandled rejection', detail);
});

function showError(title, detail) {
	if (typeof window.kruddShowError === 'function') {
		window.kruddShowError(title, detail);
		return;
	}
	/* fallback before shell is ready */
	if (!document.body) {
		alert(title + ':\n' + detail);
		return;
	}
	var d = document.createElement('div');
	d.style.cssText = 'position:fixed;top:0;left:0;right:0;bottom:0;'
		+ 'background:rgba(0,0,0,0.9);color:#f66;'
		+ 'font:13px monospace;padding:1em;z-index:9999;'
		+ 'white-space:pre-wrap;overflow:auto;';
	d.textContent = title + ':\n' + detail;
	document.body.appendChild(d);
}
