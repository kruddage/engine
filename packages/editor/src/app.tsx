// SPDX-License-Identifier: GPL-2.0-or-later
//
// The editor application.
//
// #946 left a page that proved the package builds, serves, tests and boots an
// engine, and said out loud that it was written to be replaced. #954 replaced
// it with the shell. #948 wires that shell to a document, and this file is
// where the two meet.
//
// It does four things and no more — boot the engine, stand a document up over
// it, register the panels, and hand all of it to the shell. Everything else is
// in document/, shell/ and panels/.
//
// ## Why the document is built here and not inside the shell
//
// The shell is a frame. It routes an action id to a function and lays panels
// out; it does not know what a scene is, and nothing in it should. Building the
// document at the top and passing down the five verbs the command bar needs
// keeps that true — the alternative, a hook call inside the shell, would make
// the frame a consumer of the engine and every test of the layout a test that
// needs a wasm module.

import { engine } from "virtual:krudd-engine";

import { DocumentProvider } from "./document/document-context.js";
import { browserStore as projectStore } from "./document/project-store.js";
import { useDocumentRuntime } from "./document/use-document.js";
import { useProject } from "./document/use-project.js";
import { EngineProvider } from "./engine/engine-context.js";
import { useFrameStats } from "./engine/frame-stats.js";
import { useEngine } from "./engine/use-engine.js";
import { registerBuiltinPanels } from "./panels/index.js";
import { panels } from "./shell/panels.js";
import { Shell } from "./shell/shell.js";
import { useHint, useShell } from "./shell/use-shell.js";

/*
 * At module scope, not in an effect. The registry is read during the shell's
 * first render — registering from an effect would mean one frame of an editor
 * with no panels, and a stored layout reconciled against an empty registry,
 * which would helpfully "fix" it by dropping every panel in it.
 */
registerBuiltinPanels();

/*
 * One store for the life of the page. Rebuilding it per render would give
 * useProject a new dependency every time and rebuild the five verbs with it,
 * which in turn rebuilds the shell's action table — all to reach the same
 * localStorage.
 */
const PROJECTS = projectStore();

export function App(): React.JSX.Element {
	const { status, canvasRef, log, module } = useEngine(engine);
	const { document, error } = useDocumentRuntime({ module });
	const definitions = panels();
	const stats = useFrameStats(canvasRef.current);

	/*
	 * The hint is held here rather than inside the shell because both sides
	 * need it and one is built from the other: the shell routes File > Save to
	 * a verb, and that verb reports through the hint. One queue, two writers.
	 */
	const hint = useHint();
	const project = useProject({ document, store: PROJECTS, hint: hint.show });
	const shell = useShell(definitions, undefined, project, hint);

	return (
		<EngineProvider value={{ engine, status, log, canvasRef }}>
			<DocumentProvider value={document}>
				<Shell
					shell={shell}
					panels={definitions}
					engine={engine}
					status={status}
					stats={stats}
					boundaryError={error}
				/>
			</DocumentProvider>
		</EngineProvider>
	);
}
