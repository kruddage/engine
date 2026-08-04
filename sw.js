// SPDX-License-Identifier: GPL-2.0-or-later
//
// The service worker: an offline fallback for the last-loaded build, and the
// only place this site can express a caching policy at all.
//
// GitHub Pages serves everything under its own fixed Cache-Control and honours
// no per-file override — there is no _headers file, no way to say "revalidate
// the document, keep the module for a year". So the split that would elsewhere
// be two response headers is written here instead, as two request paths:
//
//   index.html   network-first. It is the one artifact whose name never
//                changes, so a cached copy is the one thing that can pin a
//                browser to a dead build. Cache is the offline fallback only.
//   index.*.js   cache-first, never revalidated. These carry the build hash in
//   index.*.wasm their names, so a given URL's bytes are fixed forever and a
//                new build asks for different URLs.
//
// This file's own name is never hashed, and must not be: the page registers it
// by literal name. @kruddage/engine records that as `cacheBusting: false` in
// its artifact contract, which is what stops the staging step renaming it.
// Its *contents* carry the build hash instead — e04e4e71, substituted
// by the same configure-file pass that stamps shell.html.in — which is what
// makes each deploy a byte-different script the browser will actually install,
// and what names the cache that script takes over.
//
// The failure this shape exists to prevent was all three of those missing at
// once: a fixed cache name no deploy invalidated, a fixed script the browser
// therefore never re-installed, and a cache-first read that served the first
// index.html this origin ever saw. Its worst part was silent — navigation reads
// matched with `ignoreSearch: true` while writes keyed on the full URL, so
// `?anything=1` returned the oldest cached document and filed the fresh
// response under a key nothing would ever read. Every attempt to bust the cache
// from the URL bar made the cache bigger and changed nothing.

const BUILD = "e04e4e71";
const CACHE_NAME = `krudd-shell-${BUILD}`;

/* One key for the document, whatever URL asked for it.
 *
 * A navigation's query names the project (?game=ducks) and the renderer
 * (?renderer=webgl), and every one of them fetches the same index.html — the
 * page reads the query at runtime. So all of them share a cache entry, which is
 * what lets one visit prime the shell for the rest.
 *
 * The previous worker got this half right and the other half wrong: it read
 * with `ignoreSearch` but wrote under the request URL, so each distinct query
 * added an entry that would never be read while the oldest one kept answering.
 * Normalising on *write* is what makes the entry singular rather than merely
 * first. */
const SHELL_KEY = new URL("index.html", self.registration.scope).href;

/* Immutable iff the name carries this build's stem — the same stem the staging
 * step renames index.js/index.wasm with, and that the shell's Module.locateFile
 * hook applies at runtime. Matching on the current build rather than on hash
 * shape means a stale URL can never be mistaken for an immutable one; it would
 * be in a foreign cache anyway, and activate deletes those. */
function isImmutable(url) {
	return (
		url.pathname.endsWith(`.${BUILD}.js`) ||
		url.pathname.endsWith(`.${BUILD}.wasm`)
	);
}

self.addEventListener("install", () => {
	self.skipWaiting();
});

/* Drop every cache but this build's. Without this the caches were cumulative:
 * each deploy added a full set of hashed artifacts beside all of its
 * predecessors, nothing ever removed one, and a phone eventually met the origin
 * quota — where the browser evicts the whole origin at once rather than the
 * oldest entry. */
self.addEventListener("activate", (event) => {
	event.waitUntil(
		caches
			.keys()
			.then((names) =>
				Promise.all(
					names
						.filter((name) => name !== CACHE_NAME)
						.map((name) => caches.delete(name))
				)
			)
			.then(() => self.clients.claim())
	);
});

/* Network-first, cache as the offline fallback.
 *
 * The cost is real and deliberate: every launch waits for the document. It is
 * ~30 KiB, and it is the only thing standing between a phone and a build from
 * three deploys ago — which is the trade the previous worker made in the other
 * direction and got wrong.
 *
 * A redirected response is not cached. Responding to a navigation with one
 * throws in the browser, so caching it would poison the fallback with an entry
 * that fails exactly when it is needed: offline. */
async function networkFirst(request) {
	const cache = await caches.open(CACHE_NAME);
	try {
		const response = await fetch(request);
		if (response.ok && !response.redirected) {
			await cache.put(SHELL_KEY, response.clone());
		}
		return response;
	} catch (offline) {
		const cached = await cache.match(SHELL_KEY);
		if (cached) return cached;
		throw offline;
	}
}

/* Cache-first with no revalidation: for a URL carrying the build hash there is
 * nothing to revalidate against. */
async function cacheFirst(request) {
	const cache = await caches.open(CACHE_NAME);
	const cached = await cache.match(request);
	if (cached) return cached;

	const response = await fetch(request);
	if (response.ok) await cache.put(request, response.clone());
	return response;
}

/* Everything else: the icons, the webmanifest, and the runtime asset directory
 * (assets/*.scm and the projects.json naming them). Unhashed names whose bytes
 * do change between builds — but the cache is per-build and empty when this
 * worker takes over, so the first read of each is a network read and the
 * staleness window closes inside the build that opened it. */
async function staleWhileRevalidate(request) {
	const cache = await caches.open(CACHE_NAME);
	const cached = await cache.match(request);

	/* The catch has to resolve to the cached copy rather than rethrow whenever
	 * there is one: when `cached` is returned below, this promise is left
	 * running with nobody awaiting it, and a rejection there is an unhandled
	 * one. With nothing cached it is the only response there is, so it throws
	 * and the browser reports the failure it actually had. */
	const network = fetch(request)
		.then((response) => {
			if (response.ok) cache.put(request, response.clone());
			return response;
		})
		.catch((offline) => {
			if (cached) return cached;
			throw offline;
		});

	return cached || network;
}

self.addEventListener("fetch", (event) => {
	const request = event.request;
	if (request.method !== "GET") return;

	/* Cross-origin requests are left to the browser. Nothing this page loads is
	 * off-origin, and caching an opaque response would store something we
	 * cannot inspect against a quota we cannot see. */
	const url = new URL(request.url);
	if (url.origin !== self.location.origin) return;

	if (request.mode === "navigate") {
		event.respondWith(networkFirst(request));
	} else if (isImmutable(url)) {
		event.respondWith(cacheFirst(request));
	} else {
		event.respondWith(staleWhileRevalidate(request));
	}
});
