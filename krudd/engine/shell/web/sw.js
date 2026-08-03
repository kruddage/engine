// SPDX-License-Identifier: GPL-2.0-or-later
//
// Minimal service worker: caches the app shell and its hashed JS/WASM/asset
// requests so the last-loaded build keeps working offline. Filenames for
// index.js/index.wasm are hashed per commit (see packages/site), so there's no
// staleness to guard against — a fresh deploy simply requests different URLs,
// which just populate new cache entries alongside the old.
//
// This file's own name is never hashed, and must not be: the page registers it
// by literal name. @kruddage/engine records that as `cacheBusting: false` in
// its artifact contract, which is what stops the staging step renaming it.

const CACHE_NAME = "krudd-shell-v1";

self.addEventListener("install", () => {
	self.skipWaiting();
});

self.addEventListener("activate", (event) => {
	event.waitUntil(self.clients.claim());
});

self.addEventListener("fetch", (event) => {
	if (event.request.method !== "GET") return;

	// A navigation's query names the project (?game=ducks) and the renderer
	// (?renderer=webgl), and every one of them fetches the same index.html —
	// the page reads the query at runtime. So navigations match ignoring the
	// query: one visit primes the shell for all of them, and opening a
	// different project offline finds the cached page instead of missing on a
	// query string this browser happens not to have seen yet.
	const match = event.request.mode === "navigate" ? { ignoreSearch: true } : {};

	event.respondWith(
		caches.open(CACHE_NAME).then((cache) =>
			cache.match(event.request, match).then((cached) => {
				const network = fetch(event.request)
					.then((response) => {
						if (response.ok) cache.put(event.request, response.clone());
						return response;
					})
					.catch(() => cached);
				return cached || network;
			})
		)
	);
});
