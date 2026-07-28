// SPDX-License-Identifier: GPL-2.0-or-later
//
// The editor application.
//
// #946 left a page that proved the package builds, serves, tests and boots an
// engine, and said out loud that it was written to be replaced. #954 replaces
// it: the shell is the app now, the viewport keeps the canvas and the useEngine
// hook, and the identity block became the toolbar's badges and the status
// strip.
//
// This file does three things and no more — boot the engine, register the
// panels, and hand both to the shell. Everything else is in shell/ and panels/.

import { engine } from "virtual:krudd-engine";

import { EngineProvider } from "./engine/engine-context.js";
import { useFrameStats } from "./engine/frame-stats.js";
import { useEngine } from "./engine/use-engine.js";
import { registerBuiltinPanels } from "./panels/index.js";
import { panels } from "./shell/panels.js";
import { Shell } from "./shell/shell.js";
import { useShell } from "./shell/use-shell.js";

/*
 * At module scope, not in an effect. The registry is read during the shell's
 * first render — registering from an effect would mean one frame of an editor
 * with no panels, and a stored layout reconciled against an empty registry,
 * which would helpfully "fix" it by dropping every panel in it.
 */
registerBuiltinPanels();

export function App(): React.JSX.Element {
	const { status, canvasRef, log } = useEngine(engine);
	const definitions = panels();
	const shell = useShell(definitions);
	const stats = useFrameStats(canvasRef.current);

	return (
		<EngineProvider value={{ engine, status, log, canvasRef }}>
			<Shell
				shell={shell}
				panels={definitions}
				engine={engine}
				status={status}
				stats={stats}
			/>
		</EngineProvider>
	);
}
