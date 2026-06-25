/* SPDX-License-Identifier: LGPL-2.1-or-later */
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
