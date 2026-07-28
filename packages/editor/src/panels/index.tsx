// SPDX-License-Identifier: GPL-2.0-or-later
//
// The panels this build has, and where they start.
//
// Registration happens here, at module scope, and the shell reads the registry
// — so #950, #951 and #952 add a panel by adding a file and a line to this
// list, and touch no part of the frame. That is the whole point of the registry
// and it is the criterion this file exists to satisfy.
//
// Every panel below except the viewport is a placeholder, and each says so on
// screen with the issue that fills it in. They are empty in the shipped editor
// today too — that is #954's premise, not a shortcut it took.

import { ENGINE_CANVAS_ID } from "../engine/boot.js";
import { useEngineContext } from "../engine/engine-context.js";
import { registerPanel } from "../shell/panels.js";
import { Placeholder } from "./placeholder.js";
import * as Tabs from "@radix-ui/react-tabs";

/* ------------------------------------------------------------------ *
 * The viewport deck
 * ------------------------------------------------------------------ */

/*
 * The canvas, and the one panel that is not a placeholder.
 *
 * The id is a hard contract with the C tree — ten call sites look the element
 * up by `#canvas` for the GL context and for every pointer callback. See
 * ENGINE_CANVAS_ID; #949 keeps it when it takes the canvas over properly.
 */
function Viewport(): React.JSX.Element {
	const { canvasRef, engine, status } = useEngineContext();

	return (
		<div className="viewport" data-testid="viewport">
			<canvas
				ref={canvasRef}
				id={ENGINE_CANVAS_ID}
				data-testid="engine-canvas"
				/* A starting point, not a layout — the engine owns this
				 * element's size from its first tick. */
				width={1280}
				height={720}
			/>
			{!engine.built && <UnbuiltNotice command={engine.buildCommand} />}
			{engine.built && status.phase === "failed" && (
				<p className="viewport__error" role="alert" data-testid="viewport-error">
					{status.error ?? "the engine failed to start"}
				</p>
			)}
		</div>
	);
}

/*
 * Said plainly rather than rendered as an empty box. A contributor working on
 * the shell has no reason to have emsdk installed, and the difference between
 * "you have not built the engine" and "the editor is broken" is one this panel
 * is the only thing positioned to explain.
 */
function UnbuiltNotice({ command }: { command: string }): React.JSX.Element {
	return (
		<section className="unbuilt" data-testid="engine-unbuilt" role="status">
			<h2>The engine is not built</h2>
			<p>
				The shell is running, but there is no WASM module to boot. Build it
				and reload — this needs emsdk on your PATH:
			</p>
			<pre>{command}</pre>
		</section>
	);
}

/* ------------------------------------------------------------------ *
 * The docked panels
 * ------------------------------------------------------------------ */

function Outliner(): React.JSX.Element {
	return (
		<Placeholder
			heading="Scene Tree"
			blurb="The entity hierarchy of the open project — pick a node to edit it in the Inspector."
			issue={950}
		/>
	);
}

/*
 * One panel with a tab group, not three docks.
 *
 * #948 makes this call and #954 lands it, because it is a layout decision
 * rather than a contents decision — and the moment to make it is while there is
 * nothing in the tabs to migrate. The selection will decide which tab you want
 * once there is a selection to have.
 */
function Inspector(): React.JSX.Element {
	return (
		<Tabs.Root className="inspector" defaultValue="entity" data-testid="inspector">
			<Tabs.List className="inspector__tabs" aria-label="Inspector">
				<Tabs.Trigger className="inspector__tab" value="entity">
					Entity
				</Tabs.Trigger>
				<Tabs.Trigger className="inspector__tab" value="material">
					Material
				</Tabs.Trigger>
				<Tabs.Trigger className="inspector__tab" value="script">
					Script
				</Tabs.Trigger>
			</Tabs.List>
			<Tabs.Content value="entity" className="inspector__body">
				<Placeholder
					heading="Inspector"
					blurb="Components and properties of the selected entity, written back to the project files."
					issue={951}
				/>
			</Tabs.Content>
			<Tabs.Content value="material" className="inspector__body">
				<Placeholder
					heading="Material"
					blurb="The selected entity's material parameters, derived from the asset rather than hand-written beside it."
					issue={951}
				/>
			</Tabs.Content>
			<Tabs.Content value="script" className="inspector__body">
				<Placeholder
					heading="Script"
					blurb="The behaviour script bound to the selected entity, and the parameters it declares."
					issue={951}
				/>
			</Tabs.Content>
		</Tabs.Root>
	);
}

function Assets(): React.JSX.Element {
	return (
		<Placeholder
			heading="Asset Browser"
			blurb="Meshes, textures, sounds and scenes in the project, ready to drag into the scene."
			issue={952}
		/>
	);
}

/*
 * The console is the one docked panel with something real in it, and that is
 * not a special case — the engine's stdout has crossed to the page since long
 * before this shell (`shell.html.in:1379`), so showing it needs no boundary.
 *
 * The REPL half does. It says so rather than rendering an input that would do
 * nothing.
 */
function Console(): React.JSX.Element {
	const { log } = useEngineContext();

	return (
		<section className="console" data-testid="console">
			<pre className="console__log" data-testid="console-log">
				{log.length === 0 ? "The engine has not said anything yet." : log.join("\n")}
			</pre>
			<p className="console__note">
				Output only. The S7 REPL needs the boundary —{" "}
				<a
					href="https://github.com/kruddage/engine/issues/945"
					rel="noreferrer"
					target="_blank"
				>
					#945
				</a>
				.
			</p>
		</section>
	);
}

/*
 * A panel, not a button. It is slow, it fails, and it needs somewhere to say
 * so — #948's call, and it goes in now so that nobody later has to argue for
 * turning a toolbar button into a panel.
 */
function Build(): React.JSX.Element {
	return (
		<Placeholder
			heading="Build"
			blurb="Asset and shader builds, what they are doing, and what they said when they failed."
			issue={952}
		/>
	);
}

/* ------------------------------------------------------------------ *
 * Registration
 * ------------------------------------------------------------------ */

export function registerBuiltinPanels(): void {
	registerPanel({
		id: "viewport",
		title: "Viewport",
		home: "deck",
		/* Neither, and both for the same reason: the engine looks its canvas up
		 * by selector, so a viewport that unmounts or moves takes the GL
		 * context with it. See the note on PanelSlot. */
		hideable: false,
		movable: false,
		render: () => <Viewport />,
	});
	registerPanel({
		id: "outliner",
		title: "Outliner",
		home: "left",
		render: () => <Outliner />,
	});
	registerPanel({
		id: "inspector",
		title: "Inspector",
		home: "right",
		render: () => <Inspector />,
	});
	registerPanel({
		id: "assets",
		title: "Assets",
		home: "bottom",
		render: () => <Assets />,
	});
	registerPanel({
		id: "console",
		title: "Console",
		home: "bottom",
		render: () => <Console />,
	});
	registerPanel({
		id: "build",
		title: "Build",
		home: "bottom",
		render: () => <Build />,
	});
}
