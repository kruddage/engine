// SPDX-License-Identifier: GPL-2.0-or-later
//
// The viewport: where the editor stops being DOM and starts being the engine.
//
// ## Who owns the canvas
//
// Nobody in React, in the sense that matters. The element is rendered here and
// it is never conditionally rendered, never keyed, never moved between parents
// and never unmounted — the panel is registered `hideable: false, movable:
// false` for exactly that reason. Everything that varies is a *sibling* of it.
//
// That is the whole mechanism, and it is deliberately not a portal, a ref
// cache or a manual `appendChild`. React will not remount a node whose position
// in the tree never changes, so the ordinary rules are enough; the elaborate
// versions of this exist to survive a canvas that moves, and the right answer
// to a canvas that moves is that it does not.
//
// ## What is drawn by whom, and why
//
// **Everything inside the viewport is drawn by the engine.** Not one pixel of
// the scene, the selection, the grid, the gizmo or the axis indicator is DOM.
//
// - **Selection** is `render/scene_renderer`'s outline pass: the selected mesh
//   into a silhouette mask, then a full-screen edge detect that paints a border
//   where the mask's edge is — through the frame graph, against the same depth
//   buffer the scene was drawn into. It predates this issue and this issue
//   enables it, by lighting editor mode.
// - **Gizmo, grid and axis indicator** are `ui/gizmo`'s kruddgui overlay, which
//   is #944's Q4 decision applied: kruddgui is the game's GUI, it draws in the
//   canvas, and the editor's gizmos are its second tenant.
//
// The alternative — an SVG overlay projected from a view·projection read across
// the boundary — was rejected on two grounds, and the first is the one that
// matters: it is always one frame stale, so a gizmo would swim behind the mesh
// it is attached to during every orbit. The second is that it would put a
// second copy of the camera's arithmetic in TypeScript, and a hit-test that
// drifts from the draw is #813's finding about walls you can see and walk
// through.
//
// What *is* DOM here is chrome that sits beside the picture: the mode buttons,
// the snap field, the play/edit switch. Those are text and inputs, which is
// what the DOM is for and what immediate-mode is bad at.
//
// ## One view, not four
//
// #949 asks whether the deck holds one view or several and asks for the answer
// to be stated. **It holds one.** Worldcraft's four-pane arrangement is the
// reference and it is not free: four cameras, four picks and four gizmo hit
// tests a frame, each needing its own viewport size across a boundary designed
// around there being one. The engine draws through a single scene camera today
// (`render/scene_renderer`'s `g_cam`), so a second pane is a renderer change
// rather than an editor one — and it is a renderer change nobody has asked for
// yet. When somebody does, it arrives as its own issue with its own case, not
// as a shape this panel was built around on the chance.

import { ENGINE_CANVAS_ID } from "../engine/boot.js";
import { useEngineContext } from "../engine/engine-context.js";
import { GIZMO_MODE } from "../document/commands.js";
import { useOptionalDocument } from "../document/document-context.js";
import { ViewportBar } from "./viewport-bar.js";
import {
	useCanvasSize,
	useViewportPointer,
	useViewportState,
} from "./use-viewport.js";
import { useState } from "react";

export function Viewport(): React.JSX.Element {
	const { canvasRef, engine, status } = useEngineContext();
	const document = useOptionalDocument();

	/*
	 * The canvas element as state, not as the ref's current value.
	 *
	 * A ref's mutation does not re-render, so hooks that need the element
	 * would read null on the render that mounted it and never hear otherwise.
	 * The callback ref below publishes it once, which is the one render that
	 * has to happen for the observers to attach.
	 */
	const [canvas, setCanvas] = useState<HTMLCanvasElement | null>(null);

	return (
		<div className="viewport" data-testid="viewport">
			{document !== null && <ViewportBar />}
			<CanvasHost canvasRef={canvasRef} onElement={setCanvas} />
			{document !== null && <PointerSurface canvas={canvas} />}
			{!engine.built && <UnbuiltNotice command={engine.buildCommand} />}
			{engine.built && status.phase === "failed" && (
				<p className="viewport__error" role="alert" data-testid="viewport-error">
					{status.error ?? "the engine failed to start"}
				</p>
			)}
		</div>
	);
}

/**
 * The canvas, and nothing else in this component ever.
 *
 * Split out so that nothing which re-renders — the bar, the pointer surface,
 * the status — shares a component with the element that must not be
 * re-created.
 *
 * Note what the guarantee actually rests on, because it is not this split: it
 * is that the element's *position* in the tree never changes. The siblings
 * around it are `{condition && <X />}`, and a false branch still occupies its
 * index, so the canvas stays at the same index whether or not there is a
 * document yet. React reconciles by index for unkeyed children, so it updates
 * this node rather than replacing it — which is what keeps the GL context
 * alive across the null-to-document transition.
 */
function CanvasHost({
	canvasRef,
	onElement,
}: {
	canvasRef: React.RefObject<HTMLCanvasElement | null>;
	onElement: (element: HTMLCanvasElement | null) => void;
}): React.JSX.Element {
	return (
		<canvas
			/*
			 * Two consumers of one element: `canvasRef` is what boot.ts hands
			 * emscripten as `Module.canvas`, and `onElement` is what this
			 * panel measures and binds pointers to. A callback ref feeds both
			 * rather than either of them being a second lookup by id.
			 */
			ref={(element) => {
				canvasRef.current = element;
				onElement(element);
			}}
			/*
			 * A hard contract with the C tree, not a convention: ten call sites
			 * find this element by `#canvas` for the GL context and for every
			 * pointer callback. See ENGINE_CANVAS_ID — renaming it costs a GL
			 * context and reports it nowhere near the cause.
			 */
			id={ENGINE_CANVAS_ID}
			data-testid="engine-canvas"
			/*
			 * A starting point, not a layout. useCanvasSize sets the real
			 * backing store from the measured rect and the device pixel ratio
			 * on the first frame and after every resize.
			 */
			width={1280}
			height={720}
		/>
	);
}

/**
 * The element pointer events land on.
 *
 * Over the canvas rather than on it, and that is the pointer-capture
 * arbitration #944's Q4 comment added to #945: a React popover or drag ghost
 * floating over the viewport must not let a gizmo drag start underneath it.
 * DOM chrome that overlaps this surface eats its own events by sitting above it
 * in the stacking order, which is the browser's own answer to the question and
 * a better one than a hit-test we would have to maintain.
 *
 * The canvas itself cannot be that element: emscripten registers its own
 * listeners on it, and a React handler bound to the same node would leave two
 * things reading one press. This sits above it and stops the events there.
 */
function PointerSurface({
	canvas,
}: {
	canvas: HTMLCanvasElement | null;
}): React.JSX.Element {
	const state = useViewportState();
	const document = useOptionalDocument();
	const size = useCanvasSize(canvas, (width, height) => {
		document?.dispatch("viewport.size", { width, height });
	});
	const pointer = useViewportPointer(canvas, state, size);

	return (
		<div
			className="viewport__surface"
			data-testid="viewport-surface"
			data-phase={pointer.phase}
			/*
			 * The size the *engine* says it is picking against, which is not
			 * necessarily the size this panel last reported — a dock collapsed
			 * to nothing reports zero and the engine declines it.
			 *
			 * Here rather than in a global because a click landing somewhere
			 * other than where it was aimed is the hardest bug in this panel to
			 * see, and the two numbers disagreeing is what it looks like. The
			 * browser suite asserts on it for the same reason.
			 */
			data-engine-size={
				state ? `${Math.round(state.width)}x${Math.round(state.height)}` : ""
			}
			/*
			 * Focusable, so the canvas can hold keyboard focus for the
			 * shortcuts below without stealing it from a field in a dock. The
			 * shell's own shortcuts stay the shell's — see the note on
			 * onKeyDown.
			 */
			tabIndex={0}
			role="application"
			aria-label="Viewport"
			onPointerDown={pointer.onPointerDown}
			onPointerMove={pointer.onPointerMove}
			onPointerUp={pointer.onPointerUp}
			onPointerCancel={pointer.onPointerCancel}
			onWheel={pointer.onWheel}
			/*
			 * The context menu is suppressed because the secondary button is
			 * not ours — the machine ignores it — and a menu appearing over a
			 * drag the reader is halfway through is worse than no menu.
			 */
			onContextMenu={(event) => event.preventDefault()}
			onKeyDown={(event) => keyDown(event, document)}
		/>
	);
}

/*
 * How far one key press moves the camera.
 *
 * A step rather than a rate, because a held key repeats and the browser's own
 * repeat rate is the reader's setting — building a second acceleration curve on
 * top of it would fight the one they chose.
 */
const KEY_ORBIT_RADIANS = 0.08;
const KEY_PAN_FRACTION = 0.06;
const KEY_DOLLY = 0.1;

/*
 * The viewport's own keys, and only its own.
 *
 * #949 asks that input over the canvas not fight the shell's shortcuts and that
 * the owner be stated. It is stated here: **the shell owns every accelerator
 * with a modifier, and the viewport owns unmodified keys while it has focus.**
 * That line holds because every action in the shell's table carries one
 * (`src/shell/commands.ts`), so the two sets cannot overlap by construction
 * rather than by inspection — Ctrl+Z is the shell's undo wherever focus is, and
 * G/R/S switch gizmo mode only when the reader is pointing at the scene.
 *
 * Shift is the one modifier taken here, and it is taken from nothing: no shell
 * action binds Shift alone.
 *
 * The keys that are handled stop here; everything else is left to bubble, which
 * is what keeps the shell's table the one place accelerators live.
 */
function keyDown(
	event: React.KeyboardEvent,
	document: ReturnType<typeof useOptionalDocument>
): void {
	if (document === null) return;
	if (event.ctrlKey || event.metaKey || event.altKey) return;

	const handled = () => {
		event.preventDefault();
		event.stopPropagation();
	};

	/*
	 * The arrows are the keyboard's camera, and they are here because #949
	 * asks for orbit, pan and zoom to work with the keyboard as well as the
	 * mouse — a viewport reachable only by dragging is one a reader who cannot
	 * drag cannot use at all.
	 */
	const arrow: Record<string, [number, number]> = {
		ArrowLeft: [-1, 0],
		ArrowRight: [1, 0],
		ArrowUp: [0, -1],
		ArrowDown: [0, 1],
	};
	const direction = arrow[event.key];
	if (direction) {
		const [x, y] = direction;
		if (event.shiftKey) {
			document.dispatch("camera.pan", {
				dx: -x * KEY_PAN_FRACTION,
				dy: y * KEY_PAN_FRACTION,
			});
		} else {
			document.dispatch("camera.orbit", {
				dyaw: -x * KEY_ORBIT_RADIANS,
				dpitch: -y * KEY_ORBIT_RADIANS,
			});
		}
		return handled();
	}

	switch (event.key) {
		/* Both spellings of each: a numeric keypad reports "+" and "-", the
		 * main row reports "=" and "-" without shift. */
		case "+":
		case "=":
			document.dispatch("camera.dolly", { amount: KEY_DOLLY });
			return handled();
		case "-":
		case "_":
			document.dispatch("camera.dolly", { amount: -KEY_DOLLY });
			return handled();
		default:
			break;
	}

	switch (event.key.toLowerCase()) {
		case "g":
			document.dispatch("gizmo.mode", { mode: GIZMO_MODE.TRANSLATE });
			return handled();
		case "r":
			document.dispatch("gizmo.mode", { mode: GIZMO_MODE.ROTATE });
			return handled();
		case "s":
			document.dispatch("gizmo.mode", { mode: GIZMO_MODE.SCALE });
			return handled();
		case "f":
			/* Frame the selection. -1 is resolved by the engine, so this
			 * cannot disagree with it about what is selected. */
			document.dispatch("camera.frame", {});
			return handled();
		case "home":
			/* Back to the scene's authored camera, whatever the reader did. */
			document.dispatch("camera.reset", {});
			return handled();
		case "escape":
			/* Handled by the pointer machine when a drag is open; harmless
			 * otherwise. Left to bubble so a modal above can still take it. */
			return;
		default:
			return;
	}
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
