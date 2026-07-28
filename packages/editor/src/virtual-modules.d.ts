// SPDX-License-Identifier: GPL-2.0-or-later
//
// The virtual module build/engine-artifacts.mts emits, declared for TypeScript.
//
// The plugin builds the value; this file only says what shape it has, and it
// says so by pointing at the same EngineInfo the plugin imports. There is one
// definition of that type in this package and all three readers use it.

declare module "virtual:krudd-engine" {
	import type { EngineInfo } from "./engine/types.js";
	export const engine: EngineInfo;
}
