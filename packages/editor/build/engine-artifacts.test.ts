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

		expect(info.base).toBe(ENGINE_BASE);
		expect(info.buildCommand).toContain("@kruddage/engine");
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
