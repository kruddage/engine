// SPDX-License-Identifier: GPL-2.0-or-later
//
// What a press over the canvas turns out to mean.
//
// ## One owner, and it is this
//
// #949 asks the question directly: DOM events land on the canvas, the engine
// needs them in world terms, and it should be one somebody who translates. It
// is this file, and the rule is the mode:
//
// > **In editor mode the editor owns the pointer over the canvas. In game mode
// > kruddgui does.**
//
// The C side holds up the other half — `ui/viewport`'s kruddgui overlay stands
// its click-to-pick down whenever editor mode is lit, and the gizmo overlay
// draws without reading a pointer at all. So exactly one thing acts on a press,
// and which one is a flag rather than a race.
//
// ## Why a press is ambiguous for one frame
//
// A press on a gizmo handle drags the selection. A press anywhere else orbits
// the camera. Only the engine can tell them apart: the handles live in world
// space, they are hit-tested against the same view·projection the renderer
// draws with, and putting a second copy of that arithmetic in TypeScript is the
// drift #813 warns about.
//
// So the answer takes a flush. This machine holds the gesture in `pending`
// until it arrives, accumulating whatever the pointer did meanwhile, and then
// replays it into whichever branch won. One frame, which is the frame the drag
// had not moved in — the same bound #944's Q1 accepted for every other read.
//
// ## Why the machine is pure
//
// It takes events and returns intents. It calls nothing, holds no React state
// and knows nothing about the bridge, so the whole of "what does this press
// mean" is testable without a canvas, a document or a wasm module — which is
// exactly the surface #944's Q6 said a jsdom shim would lie about if it were
// written as effects instead.

import type { DragPhase } from "@kruddage/engine/bridge";

import { DRAG_PHASE } from "../document/commands.js";

/** Which mouse button, in the DOM's numbering. */
export const BUTTON = Object.freeze({
	primary: 0,
	middle: 1,
	secondary: 2,
});

/**
 * What the editor should do about a pointer event.
 *
 * Data rather than calls, so the machine stays pure and the caller stays the
 * only thing that dispatches. A step returns as many as it needs — a press
 * resolved out of `pending` emits the drag's begin and the move it had already
 * accumulated, in that order.
 */
export type ViewportIntent =
	| { kind: "gizmo"; phase: DragPhase; x: number; y: number }
	| { kind: "orbit"; dyaw: number; dpitch: number }
	| { kind: "pan"; dx: number; dy: number }
	| { kind: "dolly"; amount: number }
	| { kind: "pick"; x: number; y: number }
	/** Open the undo gesture a drag will land inside. */
	| { kind: "gesture.begin" }
	/** Close it. `commit` when something was edited, `abort` when not. */
	| { kind: "gesture.end"; commit: boolean };

export interface PointerSample {
	/** Canvas-relative pixels, top-left origin — the engine's convention. */
	x: number;
	y: number;
	button: number;
	shift: boolean;
	/** Ctrl or the Mac's Command. Pans, like every other editor. */
	meta: boolean;
}

/**
 * How far a press has to travel before it stops being a click.
 *
 * In pixels, and generous enough to survive a hand that moves on the way down.
 * Below it a press-and-release picks; above it the gesture was a drag and no
 * pick happens, so a reader who orbits does not also reselect at the end of it.
 */
export const CLICK_SLOP = 4;

type Phase =
	/* Nothing is held. */
	| { name: "idle" }
	/*
	 * Held, and the engine has not yet said whether it grabbed a handle.
	 * `serial` is the drag serial as it stood when the press went out; the
	 * answer is whatever `axis` reads once the engine's has moved past it.
	 */
	| { name: "pending"; serial: number; from: PointerSample; last: PointerSample }
	| { name: "gizmo"; from: PointerSample }
	| { name: "camera"; mode: "orbit" | "pan"; last: PointerSample; from: PointerSample };

/** How the engine's viewport state looks to this machine. */
export interface ViewportFeedback {
	dragSerial: number;
	/** -1 when no handle is held. */
	axis: number;
}

/**
 * Radians of orbit per pixel dragged.
 *
 * A full drag across a 900px viewport turns a little over half a revolution,
 * which is the ratio that makes a scene feel attached to the pointer rather
 * than either sluggish or twitchy. It is a constant rather than a fraction of
 * the viewport on purpose: a reader who resizes a dock should not find the
 * camera has changed gearing.
 */
const ORBIT_RADIANS_PER_PIXEL = 0.0045;

/**
 * How much of the eye→target gap one wheel notch closes.
 *
 * Proportional rather than absolute, so zooming in on something small stays
 * usable rather than overshooting it by a metre a notch. `deltaY` is normalized
 * by the caller, which is where the browser's three different wheel units get
 * dealt with.
 */
const DOLLY_PER_NOTCH = 0.12;

export interface PointerMachine {
	/** The phase, for tests and for the cursor the panel shows. */
	readonly phase: Phase["name"];
	/**
	 * The viewport's pixel size, so a pan can speak in the fractions
	 * `camera_api`'s pan takes. Told rather than read: the panel measures the
	 * canvas already, and a machine that measured it too would be a second
	 * answer to a question with one.
	 */
	setSize: (width: number, height: number) => void;
	down: (sample: PointerSample, serial: number) => ViewportIntent[];
	move: (sample: PointerSample) => ViewportIntent[];
	up: (sample: PointerSample) => ViewportIntent[];
	/** A pointer that left the window, or a gesture the browser cancelled. */
	cancel: () => ViewportIntent[];
	/** The reader pressed Escape mid-drag: undo the whole drag, keep the frame. */
	escape: () => ViewportIntent[];
	/** Wheel or trackpad pinch, already normalized to notches. */
	wheel: (notches: number) => ViewportIntent[];
	/**
	 * The engine's latest word. Returns the intents that were waiting on it —
	 * the drag's begin and whatever the pointer did while the answer was in
	 * flight, or nothing when the press turned out to be a camera gesture.
	 */
	settle: (feedback: ViewportFeedback) => ViewportIntent[];
}

export function createPointerMachine(): PointerMachine {
	let phase: Phase = { name: "idle" };

	/*
	 * Whether this gesture edited anything. It decides commit versus abort at
	 * the end: a camera orbit opens a gesture too (the press might have been a
	 * gizmo grab, and the gesture has to exist before the first move lands in
	 * it), and committing an empty one would put a "Transform" entry in the
	 * history that undoes nothing.
	 */
	let edited = false;

	function travelled(from: PointerSample, to: PointerSample): number {
		return Math.hypot(to.x - from.x, to.y - from.y);
	}

	/*
	 * Which camera gesture a press is, when it is one.
	 *
	 * Middle-drag and Ctrl/Cmd-drag pan; everything else orbits. The middle
	 * button is the convention every 3D tool shares, and the modifier is there
	 * because a trackpad has no middle button.
	 */
	function cameraMode(sample: PointerSample): "orbit" | "pan" {
		return sample.button === BUTTON.middle || sample.meta ? "pan" : "orbit";
	}

	function cameraIntent(
		mode: "orbit" | "pan",
		from: PointerSample,
		to: PointerSample,
		size: { width: number; height: number }
	): ViewportIntent {
		const dx = to.x - from.x;
		const dy = to.y - from.y;
		if (mode === "pan") {
			/*
			 * Fractions of the viewport, which is what camera_api's pan takes
			 * — it scales them by how far the target sits, so the framed point
			 * tracks the pointer at any distance. Negated because dragging
			 * right moves the *scene* right, which moves the camera left.
			 */
			return {
				kind: "pan",
				dx: -dx / Math.max(1, size.width),
				dy: dy / Math.max(1, size.height),
			};
		}
		return {
			kind: "orbit",
			dyaw: -dx * ORBIT_RADIANS_PER_PIXEL,
			dpitch: -dy * ORBIT_RADIANS_PER_PIXEL,
		};
	}

	/* The size the deltas above are measured against. Set by the panel. */
	let size = { width: 1, height: 1 };

	const machine: PointerMachine = {
		get phase() {
			return phase.name;
		},

		setSize(width: number, height: number) {
			size = { width, height };
		},

		down(sample, serial) {
			if (phase.name !== "idle") return [];
			/*
			 * The secondary button is the context menu's, and taking it here
			 * would mean a right-click both orbited and opened a menu.
			 */
			if (sample.button === BUTTON.secondary) return [];

			edited = false;
			phase = { name: "pending", serial, from: sample, last: sample };
			return [
				/*
				 * Opened before the answer, not after. If the press did grab a
				 * handle, the first move lands one flush later and has to land
				 * *inside* the gesture — a gesture opened after it would leave
				 * that first move as a history entry of its own, and the drag
				 * would take two undos.
				 */
				{ kind: "gesture.begin" },
				{ kind: "gizmo", phase: DRAG_PHASE.BEGIN, x: sample.x, y: sample.y },
			];
		},

		move(sample) {
			switch (phase.name) {
				case "idle":
					return [];
				case "pending":
					/* Remembered, not acted on. Replayed by settle(). */
					phase = { ...phase, last: sample };
					return [];
				case "gizmo":
					edited = true;
					return [
						{
							kind: "gizmo",
							phase: DRAG_PHASE.MOVE,
							x: sample.x,
							y: sample.y,
						},
					];
				case "camera": {
					const intent = cameraIntent(
						phase.mode,
						phase.last,
						sample,
						size
					);
					phase = { ...phase, last: sample };
					return [intent];
				}
			}
		},

		up(sample) {
			const was = phase;
			phase = { name: "idle" };

			switch (was.name) {
				case "idle":
					return [];
				case "pending":
					/*
					 * Released before the engine answered — a click, which is
					 * the common case and the fast one. The drag is closed
					 * either way (the engine may have opened one), and then it
					 * picks, unless the pointer wandered far enough that the
					 * reader meant to drag.
					 */
					return [
						{ kind: "gizmo", phase: DRAG_PHASE.END, x: sample.x, y: sample.y },
						{ kind: "gesture.end", commit: false },
						...(travelled(was.from, sample) <= CLICK_SLOP
							? [
									{
										kind: "pick" as const,
										x: sample.x,
										y: sample.y,
									},
								]
							: []),
					];
				case "gizmo":
					return [
						{ kind: "gizmo", phase: DRAG_PHASE.END, x: sample.x, y: sample.y },
						{ kind: "gesture.end", commit: edited },
					];
				case "camera":
					/*
					 * A camera drag that never moved is a click that missed
					 * every handle, so it picks — which is how clicking empty
					 * space deselects.
					 */
					return [
						{ kind: "gesture.end", commit: false },
						...(travelled(was.from, sample) <= CLICK_SLOP
							? [
									{
										kind: "pick" as const,
										x: sample.x,
										y: sample.y,
									},
								]
							: []),
					];
			}
		},

		cancel() {
			const was = phase;
			phase = { name: "idle" };
			if (was.name === "idle") return [];
			return [
				{
					kind: "gizmo",
					phase: DRAG_PHASE.END,
					x: was.name === "gizmo" ? was.from.x : 0,
					y: was.name === "gizmo" ? was.from.y : 0,
				},
				/*
				 * Committed rather than aborted, when something was edited.
				 * A cancelled pointer is usually the window losing focus
				 * mid-drag, and throwing the reader's move away because they
				 * alt-tabbed is worse than keeping it — they can undo, which
				 * is exactly the affordance the gesture bought them.
				 */
				{ kind: "gesture.end", commit: edited },
			];
		},

		escape() {
			const was = phase;
			phase = { name: "idle" };
			if (was.name === "idle") return [];
			return [
				{
					kind: "gizmo",
					phase: DRAG_PHASE.END,
					x: was.name === "gizmo" ? was.from.x : 0,
					y: was.name === "gizmo" ? was.from.y : 0,
				},
				/*
				 * Aborted, and this is the one path that means it: Escape
				 * during a drag is the reader saying "not that". The engine
				 * rolls the whole gesture back, which is what having opened
				 * one at pointerdown bought.
				 */
				{ kind: "gesture.end", commit: false },
			];
		},

		wheel(notches) {
			/* A wheel during a drag would fight it. Ignored rather than
			 * queued: a zoom that arrives after the drag ends is worse. */
			if (phase.name === "gizmo") return [];
			return [{ kind: "dolly", amount: -notches * DOLLY_PER_NOTCH }];
		},

		settle(feedback) {
			if (phase.name !== "pending") return [];
			/* Not answered yet. The engine's serial still stands where it did
			 * when the press went out. */
			if (feedback.dragSerial === phase.serial) return [];

			const { from, last } = phase;

			if (feedback.axis < 0) {
				/* Missed every handle: the gesture is the camera's. */
				const mode = cameraMode(from);
				phase = { name: "camera", mode, from, last };
				/*
				 * Replayed, not dropped. The pointer may have moved a long way
				 * while the answer was in flight — a fast flick can cover a
				 * hundred pixels in a frame — and a camera that ignored it
				 * would lurch when it finally caught up.
				 */
				return last === from ? [] : [cameraIntent(mode, from, last, size)];
			}

			phase = { name: "gizmo", from };
			if (last === from) return [];
			edited = true;
			return [{ kind: "gizmo", phase: DRAG_PHASE.MOVE, x: last.x, y: last.y }];
		},
	};

	return machine;
}

/**
 * A wheel event's delta, in notches.
 *
 * Browsers report wheels in pixels, lines or pages depending on the device and
 * the platform, and a zoom that used the raw number is three different speeds.
 * Normalizing here rather than at the call site keeps the one conversion in the
 * one place that has a reason to know about `deltaMode`.
 *
 * `pinch` is the trackpad. Every browser reports a two-finger pinch as a wheel
 * event with `ctrlKey` set and no key held — it is a synthesized event, not a
 * modifier the reader pressed — and the deltas are much smaller per unit of
 * finger travel, so they need their own scale or a pinch moves the camera a
 * tenth as far as it looks like it should.
 */
export function wheelNotches(
	deltaY: number,
	deltaMode: number,
	pinch = false
): number {
	if (pinch) return deltaY / PINCH_PER_NOTCH;
	/* 0 pixels, 1 lines, 2 pages — the DOM's own numbering. */
	const perNotch = deltaMode === 0 ? 100 : deltaMode === 1 ? 3 : 1;
	return deltaY / perNotch;
}

const PINCH_PER_NOTCH = 12;
