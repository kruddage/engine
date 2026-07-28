// SPDX-License-Identifier: GPL-2.0-or-later
//
// What a press over the canvas turns out to mean.
//
// The machine is pure — events in, intents out — so all of this runs with no
// canvas, no document and no wasm module. That is deliberate rather than
// convenient: #944's Q6 put pointer work in Playwright because "a shim will
// happily report a pass a browser would not", and the way to keep that true is
// for the part a shim *would* lie about (a real pointer capture over a real GL
// canvas) to stay in the browser suite, while the part that is arithmetic and
// state — which frame resolves a press, what a drag emits, whether a gesture
// commits — is tested where it can be tested exhaustively.

import { describe, expect, it } from "vitest";

import { DRAG_PHASE } from "../src/document/commands.js";
import {
	BUTTON,
	CLICK_SLOP,
	createPointerMachine,
	wheelNotches,
	type PointerSample,
	type ViewportIntent,
} from "../src/viewport/pointer.js";

function at(x: number, y: number, over: Partial<PointerSample> = {}): PointerSample {
	return { x, y, button: BUTTON.primary, shift: false, meta: false, ...over };
}

/** The kinds an intent list carries, which is usually what an assertion wants. */
function kinds(intents: ViewportIntent[]): string[] {
	return intents.map((intent) => intent.kind);
}

function only<K extends ViewportIntent["kind"]>(
	intents: ViewportIntent[],
	kind: K
): Extract<ViewportIntent, { kind: K }>[] {
	return intents.filter(
		(intent): intent is Extract<ViewportIntent, { kind: K }> =>
			intent.kind === kind
	);
}

describe("a press is held until the engine answers", () => {
	it("opens a gesture and asks the engine, and does nothing else", () => {
		const machine = createPointerMachine();
		const out = machine.down(at(100, 100), 4);

		expect(kinds(out)).toEqual(["gesture.begin", "gizmo"]);
		expect(only(out, "gizmo")[0]).toMatchObject({
			phase: DRAG_PHASE.BEGIN,
			x: 100,
			y: 100,
		});
		expect(machine.phase).toBe("pending");
	});

	it("opens the gesture before the answer, not after", () => {
		/*
		 * The ordering is the reason a drag is one undo step. If the gesture
		 * opened only once the engine confirmed a handle, the first move would
		 * already have landed outside it and become a history entry of its own
		 * — so undoing a drag would take two presses.
		 */
		const machine = createPointerMachine();
		expect(kinds(machine.down(at(10, 10), 0))[0]).toBe("gesture.begin");
	});

	it("holds moves while the answer is in flight", () => {
		const machine = createPointerMachine();
		machine.down(at(100, 100), 4);
		expect(machine.move(at(120, 100))).toEqual([]);
		expect(machine.phase).toBe("pending");
	});

	it("stays pending while the serial has not moved", () => {
		const machine = createPointerMachine();
		machine.down(at(100, 100), 4);
		expect(machine.settle({ dragSerial: 4, axis: -1 })).toEqual([]);
		expect(machine.phase).toBe("pending");
	});

	it("becomes a gizmo drag when a handle was grabbed", () => {
		const machine = createPointerMachine();
		machine.down(at(100, 100), 4);
		machine.settle({ dragSerial: 5, axis: 0 });

		expect(machine.phase).toBe("gizmo");
		const moved = machine.move(at(140, 100));
		expect(only(moved, "gizmo")[0]).toMatchObject({
			phase: DRAG_PHASE.MOVE,
			x: 140,
		});
	});

	it("becomes a camera drag when nothing was grabbed", () => {
		const machine = createPointerMachine();
		machine.down(at(100, 100), 4);
		machine.settle({ dragSerial: 5, axis: -1 });

		expect(machine.phase).toBe("camera");
		expect(kinds(machine.move(at(140, 100)))).toEqual(["orbit"]);
	});

	it("replays the movement that happened while it waited", () => {
		/*
		 * A fast flick covers real distance in the frame the answer takes. A
		 * camera that dropped it would sit still and then lurch when the next
		 * move arrived.
		 */
		const machine = createPointerMachine();
		machine.down(at(100, 100), 4);
		machine.move(at(180, 100));
		const settled = machine.settle({ dragSerial: 5, axis: -1 });

		expect(only(settled, "orbit")[0]?.dyaw).toBeLessThan(0);
	});

	it("replays it into the gizmo when a handle was grabbed", () => {
		const machine = createPointerMachine();
		machine.down(at(100, 100), 4);
		machine.move(at(180, 100));
		const settled = machine.settle({ dragSerial: 5, axis: 1 });

		expect(only(settled, "gizmo")[0]).toMatchObject({
			phase: DRAG_PHASE.MOVE,
			x: 180,
		});
	});

	it("replays nothing when the pointer never moved", () => {
		const machine = createPointerMachine();
		machine.down(at(100, 100), 4);
		expect(machine.settle({ dragSerial: 5, axis: 1 })).toEqual([]);
	});
});

describe("a click picks and a drag does not", () => {
	it("picks when the press is released without travelling", () => {
		const machine = createPointerMachine();
		machine.down(at(100, 100), 4);
		const out = machine.up(at(100 + CLICK_SLOP - 1, 100));

		expect(kinds(out)).toEqual(["gizmo", "gesture.end", "pick"]);
		expect(only(out, "gizmo")[0]?.phase).toBe(DRAG_PHASE.END);
	});

	it("does not pick when the pointer travelled first", () => {
		/*
		 * Otherwise an orbit would also reselect whatever happened to be under
		 * the pointer when the reader let go, which is never what they meant.
		 */
		const machine = createPointerMachine();
		machine.down(at(100, 100), 4);
		machine.settle({ dragSerial: 5, axis: -1 });
		machine.move(at(300, 220));
		expect(kinds(machine.up(at(300, 220)))).toEqual(["gesture.end"]);
	});

	it("picks after a camera press that never moved", () => {
		/* This is how clicking empty space deselects. */
		const machine = createPointerMachine();
		machine.down(at(100, 100), 4);
		machine.settle({ dragSerial: 5, axis: -1 });
		expect(kinds(machine.up(at(100, 100)))).toEqual(["gesture.end", "pick"]);
	});

	it("aborts rather than commits a gesture that edited nothing", () => {
		/*
		 * Every press opens a gesture, including the ones that turn out to be
		 * orbits. Committing them would put a "Transform" entry in the history
		 * for every click, and each would undo nothing.
		 */
		const machine = createPointerMachine();
		machine.down(at(100, 100), 4);
		const out = machine.up(at(100, 100));
		expect(only(out, "gesture.end")[0]?.commit).toBe(false);
	});

	it("commits a gesture that moved something", () => {
		const machine = createPointerMachine();
		machine.down(at(100, 100), 4);
		machine.settle({ dragSerial: 5, axis: 0 });
		machine.move(at(160, 100));
		const out = machine.up(at(160, 100));

		expect(only(out, "gesture.end")[0]?.commit).toBe(true);
		/* One undo step: exactly one gesture opened and exactly one closed. */
		expect(only(out, "gesture.end")).toHaveLength(1);
	});

	it("emits one gesture per drag however many moves it carried", () => {
		const machine = createPointerMachine();
		const all: ViewportIntent[] = [];

		all.push(...machine.down(at(100, 100), 4));
		all.push(...machine.settle({ dragSerial: 5, axis: 0 }));
		for (let x = 110; x <= 200; x += 10) all.push(...machine.move(at(x, 100)));
		all.push(...machine.up(at(200, 100)));

		expect(only(all, "gesture.begin")).toHaveLength(1);
		expect(only(all, "gesture.end")).toHaveLength(1);
		expect(only(all, "gizmo").length).toBeGreaterThan(5);
	});
});

describe("which camera gesture a press is", () => {
	it("orbits on the primary button", () => {
		const machine = createPointerMachine();
		machine.down(at(100, 100), 0);
		machine.settle({ dragSerial: 1, axis: -1 });
		expect(kinds(machine.move(at(150, 100)))).toEqual(["orbit"]);
	});

	it("pans on the middle button", () => {
		const machine = createPointerMachine();
		machine.down(at(100, 100, { button: BUTTON.middle }), 0);
		machine.settle({ dragSerial: 1, axis: -1 });
		expect(kinds(machine.move(at(150, 100)))).toEqual(["pan"]);
	});

	it("pans on ctrl or command, for a trackpad with no middle button", () => {
		const machine = createPointerMachine();
		machine.down(at(100, 100, { meta: true }), 0);
		machine.settle({ dragSerial: 1, axis: -1 });
		expect(kinds(machine.move(at(150, 100)))).toEqual(["pan"]);
	});

	it("ignores the secondary button entirely", () => {
		/* It belongs to the context menu; taking it would mean a right-click
		 * both orbited and opened one. */
		const machine = createPointerMachine();
		expect(machine.down(at(100, 100, { button: BUTTON.secondary }), 0)).toEqual(
			[]
		);
		expect(machine.phase).toBe("idle");
	});

	it("pans in viewport fractions, not pixels", () => {
		/*
		 * camera_api's pan takes fractions and scales them by how far the
		 * target sits, which is what makes the framed point track the pointer
		 * at any distance. Sending pixels would make panning speed depend on
		 * the dock's width.
		 */
		const machine = createPointerMachine();
		machine.setSize(800, 600);
		machine.down(at(100, 100, { button: BUTTON.middle }), 0);
		machine.settle({ dragSerial: 1, axis: -1 });
		const [intent] = only(machine.move(at(500, 100)), "pan");

		expect(intent?.dx).toBeCloseTo(-0.5, 5);
		expect(intent?.dy).toBeCloseTo(0, 5);
	});

	it("orbits by a rate that does not depend on the dock's size", () => {
		const wide = createPointerMachine();
		const narrow = createPointerMachine();
		wide.setSize(1600, 900);
		narrow.setSize(400, 300);
		for (const machine of [wide, narrow]) {
			machine.down(at(100, 100), 0);
			machine.settle({ dragSerial: 1, axis: -1 });
		}

		expect(only(wide.move(at(200, 100)), "orbit")[0]?.dyaw).toBeCloseTo(
			only(narrow.move(at(200, 100)), "orbit")[0]?.dyaw ?? NaN,
			9
		);
	});
});

describe("ending a gesture the reader did not finish", () => {
	it("commits a cancelled drag rather than throwing the move away", () => {
		/*
		 * A cancelled pointer is usually the window losing focus mid-drag.
		 * Keeping the move and letting the reader undo beats deciding for them
		 * that alt-tabbing meant "discard".
		 */
		const machine = createPointerMachine();
		machine.down(at(100, 100), 4);
		machine.settle({ dragSerial: 5, axis: 0 });
		machine.move(at(160, 100));
		const out = machine.cancel();

		expect(only(out, "gesture.end")[0]?.commit).toBe(true);
		expect(machine.phase).toBe("idle");
	});

	it("aborts on escape, which is the reader saying not that", () => {
		const machine = createPointerMachine();
		machine.down(at(100, 100), 4);
		machine.settle({ dragSerial: 5, axis: 0 });
		machine.move(at(160, 100));
		const out = machine.escape();

		expect(only(out, "gesture.end")[0]?.commit).toBe(false);
		expect(only(out, "gizmo")[0]?.phase).toBe(DRAG_PHASE.END);
	});

	it("does nothing when there is no gesture to end", () => {
		const machine = createPointerMachine();
		expect(machine.escape()).toEqual([]);
		expect(machine.cancel()).toEqual([]);
		expect(machine.up(at(0, 0))).toEqual([]);
	});

	it("ignores a second press while one is held", () => {
		const machine = createPointerMachine();
		machine.down(at(100, 100), 4);
		expect(machine.down(at(200, 200), 4)).toEqual([]);
	});
});

describe("the wheel", () => {
	it("dollies in when scrolled up", () => {
		const machine = createPointerMachine();
		const [intent] = only(machine.wheel(-1), "dolly");
		expect(intent?.amount).toBeGreaterThan(0);
	});

	it("does not fight a gizmo drag", () => {
		const machine = createPointerMachine();
		machine.down(at(100, 100), 4);
		machine.settle({ dragSerial: 5, axis: 0 });
		expect(machine.wheel(-1)).toEqual([]);
	});

	it("normalizes the browser's three wheel units to notches", () => {
		/*
		 * Chrome reports pixels, Firefox reports lines and a page-scroll device
		 * reports pages. Using deltaY raw makes the same gesture three
		 * different zoom speeds.
		 */
		expect(wheelNotches(100, 0)).toBe(1);
		expect(wheelNotches(3, 1)).toBe(1);
		expect(wheelNotches(1, 2)).toBe(1);
	});

	it("gives a trackpad pinch its own scale", () => {
		/*
		 * A pinch arrives as a wheel with ctrlKey set — synthesized, not a key
		 * the reader held — and with much smaller deltas per unit of finger
		 * travel. Sharing the wheel's scale would make a full pinch barely move
		 * the camera.
		 */
		expect(wheelNotches(12, 0, true)).toBe(1);
		expect(wheelNotches(12, 0, false)).toBeLessThan(0.2);
	});
});
