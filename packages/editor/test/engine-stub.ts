// SPDX-License-Identifier: GPL-2.0-or-later
//
// The component suite's stand-in for virtual:krudd-engine.
//
// vitest.config.ts aliases the virtual module here, so the suite runs whether
// or not the engine has been built. That is the point: whether a contributor
// has emsdk installed must not change whether the tests pass.
//
// It is typed as EngineInfo — the same type the plugin's output satisfies and
// the app consumes — so a change to the contract breaks this file rather than
// leaving a green suite asserting against a shape the app no longer receives.

import type { EngineInfo } from "../src/engine/types.js";

/**
 * A built engine, as the plugin would report one.
 *
 * `built: true` is the default because it is the case with more surface. The
 * unbuilt branch is exercised by overriding it — see app.test.tsx.
 */
export const engine: EngineInfo = {
	built: true,
	version: "19.2.1",
	exports: ["_main", "_krudd_load_game"],
	base: "engine/",
	buildCommand: "pnpm --filter @kruddage/engine run build",
};
