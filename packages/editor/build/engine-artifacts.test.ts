// @vitest-environment node
//
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The build plugin's two decisions worth testing without a server.
//
// Node, not jsdom: this is build-time code that reads the filesystem, and it
// sits beside the plugin rather than in test/ for the same reason — test/ is
// the application's suite and is typechecked without Node's types at all.
//
// Note what these do *not* assert: whether the engine happens to be built.
// This suite runs in CI's workspace job, which has no emsdk, and in the build
// job, which does. A test that pinned either would fail in the other half of
// CI, so what is checked is the shape of the answer and the invariants that
// hold in both.

import { describe, expect, it } from "vitest";

import { distDir } from "@kruddage/engine";

import {
	ENGINE_BASE,
	ENGINE_DIR,
	describeEngine,
	resolveArtifactRequest,
} from "./engine-artifacts.mjs";

describe("resolveArtifactRequest", () => {
	it("resolves an artifact into the engine's own directory", () => {
		const target = resolveArtifactRequest(`${ENGINE_BASE}index.wasm`);

		expect(target).toBe(`${distDir}/index.wasm`);
	});

	it("ignores a query string and a fragment", () => {
		expect(resolveArtifactRequest(`${ENGINE_BASE}index.js?v=2`)).toBe(
			`${distDir}/index.js`
		);
		expect(resolveArtifactRequest(`${ENGINE_BASE}index.js#x`)).toBe(
			`${distDir}/index.js`
		);
	});

	it("refuses to escape the artifact directory", () => {
		/* A dev server binds a port on a contributor's machine. Traversal is
		 * rejected by resolving and re-checking containment, not by looking for
		 * ".." in the input, so the encoded forms below fall out for free rather
		 * than needing their own patterns. */
		for (const attack of [
			"../../../etc/passwd",
			"..%2f..%2fetc%2fpasswd",
			"%2e%2e/%2e%2e/etc/passwd",
			"/etc/passwd",
		]) {
			expect(resolveArtifactRequest(ENGINE_BASE + attack)).toBeNull();
		}
	});

	it("refuses a malformed escape rather than throwing", () => {
		expect(resolveArtifactRequest(`${ENGINE_BASE}%E0%A4%A`)).toBeNull();
	});

	it("declines anything outside its own base", () => {
		expect(resolveArtifactRequest("/src/main.tsx")).toBeNull();
	});
});

describe("describeEngine", () => {
	it("always tells the app where to look and how to fix it", () => {
		const info = describeEngine();

		expect(info.base).toBe(ENGINE_DIR);
		expect(info.buildCommand).toContain("@kruddage/engine");
	});

	/*
	 * The half of the deploy bug that belongs to this file. An absolute base is
	 * indistinguishable from a relative one in every harness — the dev server
	 * and `vite preview` both serve at "/" — and wrong on a site served under a
	 * prefix, which is every deploy this repository has. Asserting the shape
	 * rather than the value is deliberate: what matters is that it cannot name
	 * the domain root, whatever the directory ends up being called.
	 */
	it("hands the app a page-relative base, never an absolute one", () => {
		expect(describeEngine().base.startsWith("/")).toBe(false);
		expect(ENGINE_DIR.startsWith("/")).toBe(false);
		/* The dev mount is the opposite case and stays absolute: it is matched
		 * against a request URL, which always begins at the root. */
		expect(ENGINE_BASE.startsWith("/")).toBe(true);
	});

	/*
	 * The same property stated as the outcome it protects, because that is the
	 * form in which it actually went wrong. boot.ts joins this base to
	 * `document.baseURI`; a deployed page's baseURI is a directory several
	 * segments down, and an absolute base silently ignores every one of them.
	 *
	 * The URL below is the real one from the report that found this — a PR
	 * preview on GitHub Pages, where the absolute form resolved to
	 * `https://kruddage.github.io/engine/index.js`. That is not even a 404: the
	 * repository is called `engine`, so it lands on the site root and gets a
	 * stale bundle with a 200, which raises no error and boots nothing.
	 */
	it("lands in the page's own directory on a deployed page", () => {
		const page = "https://kruddage.github.io/engine/pr-preview/pr-966/";

		expect(new URL(describeEngine().base + "index.js", page).href).toBe(
			"https://kruddage.github.io/engine/pr-preview/pr-966/engine/index.js"
		);
	});

	it("keeps version and exports consistent with built", () => {
		const info = describeEngine();

		/* The invariant that holds in both halves of CI: an unbuilt engine
		 * reports no version, and a built one reports one. Nothing here asserts
		 * which of the two this machine is. */
		if (info.built) {
			expect(info.version).not.toBeNull();
		} else {
			expect(info.version).toBeNull();
			expect(info.exports).toEqual([]);
		}
	});
});
