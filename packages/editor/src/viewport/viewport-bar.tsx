// SPDX-License-Identifier: GPL-2.0-or-later
//
// The viewport's own controls: gizmo mode, snapping, the grid, and the GAME /
// EDITOR switch.
//
// ## Why this is DOM and the gizmo is not
//
// The line is not "editor things are DOM". It is that a *picture of the scene*
// belongs in the canvas, where it can be depth-correct and one frame fresh, and
// that *text, buttons and number fields* belong in the DOM, where they are
// accessible, selectable and keyboard-navigable for free. kruddgui draws
// beautiful handles and would need a month of work to draw a number field a
// screen reader can use.
//
// ## It holds nothing
//
// Every value here comes from the `viewport` query and every change is a
// command. Pressing Rotate does not set a local state and then tell the engine;
// it tells the engine, and the button re-renders when the engine says the mode
// moved. That is #944's Q1 applied to a toolbar, and it is what makes this bar
// agree with a mode change made by pressing R over the canvas — or by anything
// else that ever drives the same command.
//
// The one exception is the snap field's *text*, and it is a real one: a reader
// typing "0.2" passes through "0." and "0.", parsed, is not a number. Holding
// the keystrokes locally while the parsed value goes to the engine is the only
// way a field like this can work; `draft` below is that, and it is text, not
// state about the scene.

import { useEffect, useState } from "react";

import { useDocument } from "../document/document-context.js";
import { GIZMO_MODE } from "../document/commands.js";
import { useViewportState } from "./use-viewport.js";

/** The modes, in the order they are shown, with the key that reaches each. */
const MODES = [
	{ mode: GIZMO_MODE.TRANSLATE, label: "Move", key: "G" },
	{ mode: GIZMO_MODE.ROTATE, label: "Rotate", key: "R" },
	{ mode: GIZMO_MODE.SCALE, label: "Scale", key: "S" },
] as const;

export function ViewportBar(): React.JSX.Element {
	const document = useDocument();
	const state = useViewportState();

	return (
		<div className="viewport__bar" data-testid="viewport-bar">
			<div
				className="viewport__modes"
				role="radiogroup"
				aria-label="Transform mode"
			>
				{MODES.map(({ mode, label, key }) => (
					<button
						key={label}
						type="button"
						role="radio"
						aria-checked={state?.mode === mode}
						data-testid={`gizmo-mode-${label.toLowerCase()}`}
						onClick={() => document.dispatch("gizmo.mode", { mode })}
					>
						{label}
						<kbd>{key}</kbd>
					</button>
				))}
			</div>

			<SnapField />

			<label className="viewport__toggle">
				<input
					type="checkbox"
					data-testid="viewport-grid"
					checked={state?.grid.shown ?? false}
					onChange={(event) =>
						document.dispatch("gizmo.grid", {
							shown: event.target.checked,
							spacing: state?.grid.spacing ?? 1,
						})
					}
				/>
				Grid
			</label>

			<PlaySwitch />
		</div>
	);
}

/**
 * The snap increment for the live mode.
 *
 * One field rather than three, because the three are never useful at once: a
 * reader snapping a rotation does not care what the translate increment is, and
 * three fields is three things to read past to find the one that applies. Which
 * one it edits follows the mode, which is the same thing the handles follow.
 */
function SnapField(): React.JSX.Element {
	const document = useDocument();
	const state = useViewportState();
	const mode = state?.mode ?? GIZMO_MODE.TRANSLATE;

	const live =
		mode === GIZMO_MODE.ROTATE
			? (state?.snap.rotate ?? 0)
			: mode === GIZMO_MODE.SCALE
				? (state?.snap.scale ?? 0)
				: (state?.snap.translate ?? 0);

	/*
	 * The keystrokes, not the value. See the note at the top of this file: the
	 * engine holds the number and this holds the half-typed text on the way to
	 * it. Re-seeded whenever the engine's value moves for any other reason,
	 * which is how a mode switch shows that mode's increment.
	 */
	const [draft, setDraft] = useState(String(live));
	useEffect(() => {
		setDraft(String(live));
	}, [live, mode]);

	const commit = (text: string): void => {
		setDraft(text);

		const trimmed = text.trim();
		/*
		 * `Number`, not `parseFloat`, and the empty check is not redundant.
		 * parseFloat("1abc") is 1, which would let a typo through as a value;
		 * Number("") is 0, which would turn snapping off the instant a reader
		 * selected the field and pressed delete on the way to typing something
		 * else. Both are the field mid-edit rather than an instruction, and
		 * the reader's text stays on screen either way.
		 */
		const value = trimmed === "" ? Number.NaN : Number(trimmed);
		if (!Number.isFinite(value) || value < 0) return;
		document.dispatch("gizmo.snap", {
			translate:
				mode === GIZMO_MODE.TRANSLATE ? value : (state?.snap.translate ?? 0),
			rotate: mode === GIZMO_MODE.ROTATE ? value : (state?.snap.rotate ?? 0),
			scale: mode === GIZMO_MODE.SCALE ? value : (state?.snap.scale ?? 0),
		});
	};

	return (
		<label className="viewport__snap">
			Snap
			<input
				/*
				 * Text, not `type="number"`, and it is the draft that forces
				 * it: a number input runs its own value sanitization, so the
				 * "0." a reader passes through on the way to "0.25" is
				 * discarded before this component ever sees it and the
				 * keystroke vanishes under their hands. `inputMode` still
				 * brings up the numeric keypad on a touch device, which is the
				 * half of `type="number"` worth having here. The cost is the
				 * spinner arrows, and nobody has ever nudged a snap increment
				 * by 0.1 with a spinner.
				 */
				type="text"
				inputMode="decimal"
				data-testid="viewport-snap"
				aria-label={
					mode === GIZMO_MODE.ROTATE
						? "Rotation snap, degrees"
						: mode === GIZMO_MODE.SCALE
							? "Scale snap"
							: "Translation snap, world units"
				}
				value={draft}
				onChange={(event) => commit(event.target.value)}
			/>
			{/* The unit, so "15" is not ambiguous between degrees and units. */}
			<span aria-hidden="true">
				{mode === GIZMO_MODE.ROTATE ? "°" : mode === GIZMO_MODE.SCALE ? "×" : "u"}
			</span>
		</label>
	);
}

/**
 * The GAME / EDITOR switch — the play/edit handover, and the whole of it.
 *
 * `body.editor-mode` was the entire mechanism in the shipped shell and #949
 * asks that its spirit be kept. It is: one flag, one command, and the engine
 * does everything that follows from it — pauses the simulation, lights the
 * selection outline, hands the pointer over. Nothing on this side is torn down
 * or rebuilt, which is what makes entering and leaving repeatedly cost nothing
 * and need no reload. The canvas never moves; only what the engine draws into
 * it does.
 */
function PlaySwitch(): React.JSX.Element {
	const document = useDocument();
	const state = useViewportState();
	const editing = state?.editorMode ?? false;

	return (
		<button
			type="button"
			className="viewport__play"
			data-testid="viewport-play"
			aria-pressed={!editing}
			onClick={() => document.dispatch("engine.editing", { editing: !editing })}
		>
			{editing ? "Play" : "Stop"}
		</button>
	);
}
