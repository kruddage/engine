// SPDX-License-Identifier: GPL-2.0-or-later
//
// The viewport, from inside the panel: the canvas's size, the pointer machine
// wired to the document, and the engine's answer about what a press grabbed.
//
// ## The canvas is not React's, and this is what that costs
//
// The engine looks its canvas up by selector and holds a GL context on it, so
// the element must outlive every re-render of the tree it happens to sit in.
// That is not a thing React guarantees — a remount hands the engine a detached
// element and the failure surfaces later, somewhere else, as a null in
// glCreateShader (see boot.ts's note on ENGINE_CANVAS_ID).
//
// The panel keeps it alive by never conditionally rendering it and never moving
// it: the viewport panel is registered `hideable: false, movable: false`, and
// everything that varies — the unbuilt notice, the error, the toolbar — is a
// sibling of the canvas rather than a wrapper around it. This file's job is the
// other half: measuring the element and telling the engine, without the measure
// ever becoming a reason to re-render the element itself.

import { useCallback, useEffect, useMemo, useRef, useState } from "react";

import type { ViewportState } from "@kruddage/engine/bridge";

import { useDocument, useQuery } from "../document/document-context.js";
import type { KruddDocument } from "../document/document.js";
import {
	createPointerMachine,
	wheelNotches,
	type PointerSample,
	type ViewportIntent,
} from "./pointer.js";

/** The viewport's state as the engine reports it, or null before it has. */
export function useViewportState(): ViewportState | null {
	return useQuery("viewport");
}

/**
 * Report the canvas's drawable size to the engine, and keep reporting it.
 *
 * Device pixels, not CSS pixels — but the *number* the engine wants is CSS
 * pixels, because that is what a pointer event carries. What the DPR is for is
 * the backing store: a canvas whose `width` attribute is its CSS width on a 2×
 * display renders at half resolution, and the engine sizes its own backing
 * store from what it is told.
 *
 * ResizeObserver rather than a window resize listener, because the thing that
 * moves is a dock splitter and the window does not know about it.
 */
export function useCanvasSize(
	canvas: HTMLCanvasElement | null,
	report: (width: number, height: number) => void
): { width: number; height: number } {
	const [size, setSize] = useState({ width: 0, height: 0 });

	/*
	 * A ref, so the effect below does not re-subscribe every time the caller
	 * rebuilds its callback. An observer that tore down and re-attached on
	 * every render would miss the resize that happened in between.
	 */
	const reportRef = useRef(report);
	reportRef.current = report;

	useEffect(() => {
		if (canvas === null) return;

		const measure = (): void => {
			const rect = canvas.getBoundingClientRect();
			/*
			 * A collapsed dock measures zero. Reported as nothing rather than
			 * as a size: the engine ignores a non-positive one anyway, and
			 * adopting it here would make the panel re-render its way through
			 * a divide-by-zero on the way back out.
			 */
			if (rect.width <= 0 || rect.height <= 0) return;

			const dpr = window.devicePixelRatio || 1;
			/*
			 * The backing store, rounded to whole pixels. A fractional
			 * attribute is silently truncated by the browser, and the
			 * half-pixel that goes missing is the seam a reader sees down the
			 * edge of the viewport.
			 */
			const backingWidth = Math.round(rect.width * dpr);
			const backingHeight = Math.round(rect.height * dpr);
			if (canvas.width !== backingWidth) canvas.width = backingWidth;
			if (canvas.height !== backingHeight) canvas.height = backingHeight;

			setSize((held) =>
				held.width === rect.width && held.height === rect.height
					? held
					: { width: rect.width, height: rect.height }
			);
			reportRef.current(rect.width, rect.height);
		};

		measure();
		const observer = new ResizeObserver(measure);
		observer.observe(canvas);

		/*
		 * A DPR change fires no resize on the element — dragging a window
		 * between a laptop screen and an external monitor changes the ratio
		 * while the CSS box stays exactly the same size. This is the documented
		 * way to hear about it, and it has to be re-armed after every change
		 * because the query is for one specific ratio.
		 */
		let media: MediaQueryList | null = null;
		const onDprChange = (): void => {
			measure();
			arm();
		};
		const arm = (): void => {
			media?.removeEventListener("change", onDprChange);
			media = window.matchMedia(
				`(resolution: ${window.devicePixelRatio || 1}dppx)`
			);
			media.addEventListener("change", onDprChange);
		};
		arm();

		return () => {
			observer.disconnect();
			media?.removeEventListener("change", onDprChange);
		};
	}, [canvas]);

	return size;
}

export interface ViewportPointer {
	/** Handlers to spread onto the element that covers the canvas. */
	onPointerDown: (event: React.PointerEvent) => void;
	onPointerMove: (event: React.PointerEvent) => void;
	onPointerUp: (event: React.PointerEvent) => void;
	onPointerCancel: (event: React.PointerEvent) => void;
	onWheel: (event: React.WheelEvent) => void;
	/** What the machine currently thinks it is doing. Drives the cursor. */
	phase: string;
}

/**
 * Bind the pointer machine to a document.
 *
 * Every intent the machine returns is dispatched here and nowhere else, so the
 * machine stays a pure function of events and this stays the only place that
 * knows a command table exists.
 */
export function useViewportPointer(
	canvas: HTMLCanvasElement | null,
	state: ViewportState | null,
	size: { width: number; height: number }
): ViewportPointer {
	const document = useDocument();
	const machine = useMemo(() => createPointerMachine(), []);
	const [phase, setPhase] = useState("idle");

	/*
	 * The open undo gesture, if any. A ref rather than state: it is written
	 * and read inside event handlers within one frame, and a re-render between
	 * the two would be a re-render nobody asked for.
	 */
	const gesture = useRef<ReturnType<KruddDocument["gesture"]> | null>(null);

	/*
	 * During render rather than in an effect, deliberately. The machine is a
	 * plain object, this is idempotent, and a handler that fires before the
	 * effect would otherwise pan against a stale size — which is exactly what
	 * happens on the first drag after a dock resize.
	 */
	machine.setSize(size.width, size.height);

	const run = useCallback(
		(intents: ViewportIntent[]): void => {
			for (const intent of intents) dispatch(document, gesture, intent);
			setPhase(machine.phase);
		},
		[document, machine]
	);

	/*
	 * The engine's answer to "did that press grab a handle". An effect on the
	 * serial rather than a poll, so a settle happens exactly once per answer —
	 * see ViewportState.dragSerial for why the serial carries it rather than
	 * the axis.
	 */
	const serial = state?.dragSerial ?? null;
	const axis = state?.axis ?? null;
	useEffect(() => {
		if (serial === null || axis === null) return;
		run(machine.settle({ dragSerial: serial, axis }));
	}, [serial, axis, machine, run]);

	const sample = useCallback(
		(event: React.PointerEvent | React.MouseEvent): PointerSample => {
			const rect = canvas?.getBoundingClientRect();
			return {
				/*
				 * Canvas-relative, top-left origin — the convention
				 * ray_from_screen takes and the one the gizmo projects into.
				 * Converting here rather than in the machine keeps the machine
				 * free of the DOM.
				 */
				x: event.clientX - (rect?.left ?? 0),
				y: event.clientY - (rect?.top ?? 0),
				button: event.button,
				shift: event.shiftKey,
				meta: event.ctrlKey || event.metaKey,
			};
		},
		[canvas]
	);

	return {
		phase,

		onPointerDown: (event) => {
			/*
			 * Captured on the element the press landed on, so a drag that
			 * leaves the canvas — over a dock, off the window — still delivers
			 * its moves here. Without it a gizmo drag stops the moment the
			 * pointer crosses the viewport's edge, which is exactly when a
			 * reader is dragging something to the far side of the scene.
			 */
			event.currentTarget.setPointerCapture(event.pointerId);
			run(machine.down(sample(event), state?.dragSerial ?? 0));
		},
		onPointerMove: (event) => run(machine.move(sample(event))),
		onPointerUp: (event) => run(machine.up(sample(event))),
		onPointerCancel: () => run(machine.cancel()),
		onWheel: (event) =>
			/*
			 * ctrlKey on a wheel is a trackpad pinch, not a reader holding
			 * Ctrl — every browser synthesizes it that way, and it is the only
			 * signal there is. Treating it as an ordinary wheel would make a
			 * pinch move the camera about a tenth as far as it looks like it
			 * should.
			 */
			run(
				machine.wheel(
					wheelNotches(event.deltaY, event.deltaMode, event.ctrlKey)
				)
			),
	};
}

/**
 * One intent, dispatched.
 *
 * Split out of the hook so the mapping from intent to command is one flat table
 * a reader can check against the machine's union, rather than a switch buried
 * in a closure.
 */
function dispatch(
	document: KruddDocument,
	gesture: React.RefObject<ReturnType<KruddDocument["gesture"]> | null>,
	intent: ViewportIntent
): void {
	switch (intent.kind) {
		case "gesture.begin":
			/*
			 * One at a time. A second begin without an end would leave the
			 * first open for ever, and the engine's depth would never return
			 * to zero — so the editor would look permanently mid-drag.
			 */
			gesture.current?.abort();
			gesture.current = document.gesture("Transform");
			return;
		case "gesture.end":
			if (intent.commit) gesture.current?.commit();
			else gesture.current?.abort();
			gesture.current = null;
			return;
		case "gizmo":
			document.dispatch("gizmo.drag", {
				phase: intent.phase,
				x: intent.x,
				y: intent.y,
			});
			return;
		case "orbit":
			document.dispatch("camera.orbit", {
				dyaw: intent.dyaw,
				dpitch: intent.dpitch,
			});
			return;
		case "pan":
			document.dispatch("camera.pan", { dx: intent.dx, dy: intent.dy });
			return;
		case "dolly":
			document.dispatch("camera.dolly", { amount: intent.amount });
			return;
		case "pick":
			document.dispatch("viewport.pick", { x: intent.x, y: intent.y });
			return;
	}
}
