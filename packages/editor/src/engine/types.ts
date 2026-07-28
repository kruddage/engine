// SPDX-License-Identifier: GPL-2.0-or-later
//
// The contract between the build and the app.
//
// EngineInfo is defined here, in src/, rather than beside the plugin that
// produces it. The direction matters: the application declares what it needs to
// know about the engine, and the build satisfies it. Defining it in build/ would
// have the plugin decide what the app is allowed to ask, which is backwards, and
// it would leave the test stub free to drift from both.
//
// Three things read this type — the plugin (build/engine-artifacts.mts), the
// app, and the component suite's stub (test/engine-stub.ts). A change here is a
// type error in whichever of the three did not keep up, which is the point.

/** What the app is told about the engine it is running against. */
export interface EngineInfo {
	built: boolean;
	/** The version stamped into the build, or null when unbuilt. */
	version: string | null;
	/** The function exports actually present in the module, or []. */
	exports: readonly string[];
	/** Where the loader is served from, whether or not it exists yet. */
	base: string;
	/** The one command that fixes `built: false`. */
	buildCommand: string;
}

/**
 * How far the engine has got.
 *
 * "unbuilt" is not a failure of this boot — it means there was nothing to boot,
 * and it is reported separately so the UI can tell a contributor without emsdk
 * apart from a contributor whose engine genuinely broke.
 */
export type EnginePhase =
	| "unbuilt"
	| "loading"
	| "running"
	| "ready"
	| "failed";

/** Live state of the running module, as the engine's own callbacks report it. */
export interface EngineStatus {
	phase: EnginePhase;
	/** The backend that actually went live, once one has. */
	renderer: string | null;
	/** Why the boot or the backend failed, when it did. */
	error: string | null;
}
