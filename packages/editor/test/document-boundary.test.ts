// SPDX-License-Identifier: GPL-2.0-or-later
//
// One rule, asserted against the source tree: **only `src/document/` touches
// the bridge.**
//
// #947 asks that every scene mutation go through a command and that "a panel
// that writes directly is a bug, and there is a test that catches it". This is
// that test, and it is a source-level one because there is no runtime shape
// that could catch it: `useDocument().bridge` is right there, and a panel that
// called `bridge.setName(...)` would work perfectly — right up until two panels
// disagreed about the scene and nobody could say which one had written to it.
//
// Keeping the check textual also means it fires on the import, before the
// mutation is written, which is the moment it is cheapest to reconsider.
//
// The files come through Vite's `import.meta.glob` with `query: "?raw"` rather
// than node:fs, for the same reason styles.test.ts does it: `src/` and `test/`
// deliberately have no Node types, and tsconfig leaving `node:fs` undefined is
// the guard that says so.

import { describe, expect, it } from "vitest";

const SOURCES = import.meta.glob("../src/**/*.{ts,tsx}", {
	query: "?raw",
	import: "default",
	eager: true,
}) as Record<string, string>;

/** Where the boundary may be imported from. Everything else goes through it. */
const ALLOWED = /^\.\.\/src\/document\//;

/*
 * A *value* import — `import type` is exempt and deliberately so. The engine
 * hook needs `BridgeModule` to describe the thing it hands over, and a type
 * import is erased: it cannot call anything, so it cannot mutate anything. The
 * rule this file enforces is about who can reach the boundary at runtime.
 */
const BRIDGE_IMPORT =
	/import\s+(?!type\s)[^;]*?from\s+"@kruddage\/engine\/bridge"/s;

describe("the boundary has one door", () => {
	it("finds the sources at all", () => {
		/*
		 * A glob that matched nothing would make every assertion below pass
		 * vacuously, which is the failure mode of every test shaped like this
		 * one.
		 */
		expect(Object.keys(SOURCES).length).toBeGreaterThan(10);
		expect(Object.keys(SOURCES)).toContain("../src/document/document.ts");
	});

	it("is imported for its values only from src/document", () => {
		const offenders = Object.entries(SOURCES)
			.filter(([path]) => !ALLOWED.test(path))
			.filter(([, text]) => BRIDGE_IMPORT.test(stripComments(text)))
			.map(([path]) => path);

		expect(offenders).toEqual([]);
	});

	it("has exactly one place that calls a bridge mutator", () => {
		/*
		 * The stronger half of the rule. An import can be a type import — the
		 * engine hook needs `BridgeModule` to hand the module over, and that is
		 * fine. What must not spread is the *calling*: every mutation in the
		 * application is a `run` in the command table, and a second call site
		 * would be a mutation nothing can describe, log or route.
		 */
		const mutators =
			/\bbridge\.(select|createEntity|destroyEntity|setTransform|setName|setRenderRef|setMaterialRef|setScriptRef|setPaused|loadScene|undo|redo)\s*\(/;

		const callers = Object.entries(SOURCES)
			.filter(([, text]) => mutators.test(stripComments(text)))
			.map(([path]) => path);

		expect(callers).toEqual(["../src/document/commands.ts"]);
	});

	it("keeps the shell free of engine imports entirely", () => {
		/*
		 * The shell is a frame. It routes an action id to a function and lays
		 * panels out; the day it imports the engine is the day testing the
		 * layout needs a wasm module.
		 */
		const offenders = Object.entries(SOURCES)
			.filter(([path]) => path.startsWith("../src/shell/"))
			.filter(([, text]) =>
				/import\s+(?!type\s)[^;]*?from\s+"@kruddage\/engine/s.test(
					stripComments(text)
				)
			)
			.map(([path]) => path);

		expect(offenders).toEqual([]);
	});
});

/*
 * Comments carry the forbidden strings as prose all over this package — the
 * note explaining why a panel must not import the bridge would otherwise be
 * the thing that fails the test.
 */
function stripComments(text: string): string {
	return text.replace(/\/\*[\s\S]*?\*\//g, "").replace(/^\s*\/\/.*$/gm, "");
}
