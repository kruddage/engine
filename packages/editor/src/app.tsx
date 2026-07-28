// SPDX-License-Identifier: GPL-2.0-or-later
//
// The editor application, at the stage #946 leaves it.
//
// This is deliberately not the editor. There are no docks, no panels and no
// command bar here — that is #954, which builds the shell into this package on
// top of what this file proves works. What this proves is the one thing #946
// exists to prove: the package builds, serves, tests, and boots a real engine.
//
// It is written to be replaced. When #954 lands its shell, the viewport keeps
// the canvas and the useEngine hook, the identity block becomes the status
// strip, and this file goes.

import { engine } from "virtual:krudd-engine";

import { ENGINE_CANVAS_ID } from "./engine/boot.js";
import { useEngine } from "./engine/use-engine.js";
import type { EngineStatus } from "./engine/types.js";

const PHASE_LABEL: Record<EngineStatus["phase"], string> = {
	unbuilt: "not built",
	loading: "loading…",
	running: "running",
	ready: "ready",
	failed: "failed",
};

export function App(): React.JSX.Element {
	const { status, canvasRef, log } = useEngine(engine);

	return (
		<main className="app">
			<header className="identity">
				<h1>KRUDD Editor</h1>
				<p className="subtitle">
					The package skeleton. The shell is #954; the boundary is #945.
				</p>
			</header>

			{engine.built ? (
				<EngineIdentity />
			) : (
				<UnbuiltNotice command={engine.buildCommand} />
			)}

			<section className="viewport" aria-label="Viewport">
				<canvas
					ref={canvasRef}
					/* Not cosmetic, and not ours to choose — the engine looks
					 * this element up by selector rather than taking a handle,
					 * for the GL context and for every pointer callback. See
					 * ENGINE_CANVAS_ID. */
					id={ENGINE_CANVAS_ID}
					data-testid="engine-canvas"
					/* The engine owns this element's size once it boots; the
					 * attributes are a starting point, not a layout. */
					width={1280}
					height={720}
				/>
			</section>

			<StatusStrip status={status} />

			{log.length > 0 && (
				<section className="log" aria-label="Engine output">
					<pre data-testid="engine-log">{log.join("\n")}</pre>
				</section>
			)}
		</main>
	);
}

function EngineIdentity(): React.JSX.Element {
	return (
		<dl className="engine-identity" data-testid="engine-identity">
			<dt>version</dt>
			<dd data-testid="engine-version">{engine.version}</dd>
			<dt>wasm exports</dt>
			<dd data-testid="engine-exports">
				{engine.exports.length > 0 ? engine.exports.join(", ") : "none"}
			</dd>
		</dl>
	);
}

/*
 * Said plainly rather than rendered as an empty box. A contributor working on
 * the editor's chrome has no reason to have emsdk installed, and the difference
 * between "you have not built the engine" and "the editor is broken" is one
 * this page is the only thing positioned to explain.
 */
function UnbuiltNotice({ command }: { command: string }): React.JSX.Element {
	return (
		<section className="unbuilt" data-testid="engine-unbuilt" role="status">
			<h2>The engine is not built</h2>
			<p>
				The editor is running, but there is no WASM module to boot. Build
				it and reload — this needs emsdk on your PATH:
			</p>
			<pre>{command}</pre>
		</section>
	);
}

function StatusStrip({ status }: { status: EngineStatus }): React.JSX.Element {
	return (
		<footer className="status-strip" data-testid="engine-status">
			<span data-testid="status-phase">{PHASE_LABEL[status.phase]}</span>
			<span data-testid="status-renderer">
				{status.renderer ?? "renderer — booting…"}
			</span>
			{status.error !== null && (
				<span className="status-error" data-testid="status-error">
					{status.error}
				</span>
			)}
		</footer>
	);
}
