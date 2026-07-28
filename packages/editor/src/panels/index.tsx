// SPDX-License-Identifier: GPL-2.0-or-later
//
// The panels this build has, and where they start.
//
// Registration happens here, at module scope, and the shell reads the registry
// — so #950, #951 and #952 add a panel by adding a file and a line to this
// list, and touch no part of the frame. That is the whole point of the registry
// and it is the criterion this file exists to satisfy.
//
// The viewport is `src/viewport/`, which is where the canvas handover, the
// camera and the picking live (#949) — it is the one panel big enough to be a
// directory rather than a function here.
//
// Every panel below except the viewport and the inspector is a placeholder, and
// each says so on screen with the issue that fills it in. They are empty in the
// shipped editor today too — that is #954's premise, not a shortcut it took.

import { useEngineContext } from "../engine/engine-context.js";
import { registerPanel } from "../shell/panels.js";
import { Viewport } from "../viewport/viewport.js";
import { Inspector } from "./inspector/inspector.js";
import { Placeholder } from "./placeholder.js";

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
