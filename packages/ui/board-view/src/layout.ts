// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * Where everything goes: lanes, nodes, ports and wires, in board pixels.
 *
 * Pure arithmetic over a board and a set of *measured* node sizes. No DOM,
 * which is what lets the geometry be asserted under `node:test` — and the
 * geometry is where this gets subtly wrong in ways a screenshot does not
 * catch, like a wire that lands two pixels off its port at one flow direction
 * and not the other.
 *
 * ## Measured, never guessed
 *
 * Sizes come in rather than being decided here. A node box is as tall as its
 * own content — a title, however many params, a port list — and a guessed
 * height does two things at once: it spills the text past the border, and it
 * lands every wire off its port, because the ports are placed as fractions of
 * the box. So the view measures first and calls this second.
 *
 * ## One layout that reacts
 *
 * A lane runs left-to-right where there is room and top-to-bottom where there
 * is not. Same board, same order, same wires — the wires re-route because
 * their ports move to the leading and trailing edges of whichever axis the
 * lane flows along. This is not a phone build of the editor; it is the editor,
 * measured.
 */

import type {
	Board,
	BoardNode,
	Lane,
	NodeId,
	PortName,
	Registry,
	WireId,
	WireKind,
} from "@krudd/board";
import { EXEC_IN, EXEC_OUT, kindOf, LANES } from "@krudd/board";

/** Below this viewport width, lanes stack down instead of running across. */
export const NARROW_WIDTH = 720;

/** Padding inside a lane, and around the board. */
const PAD = 16;

/** Height of a lane's label strip. */
const LABEL_HEIGHT = 26;

/** Between two nodes in a lane, along the direction the lane flows. */
const NODE_GAP = 28;

/** Between two lanes. */
const LANE_GAP = 18;

/** How far a wire bows out of its port before heading for the other end. */
const WIRE_BOW = 34;

/** Which way lanes run. */
export type Flow = "across" | "down";

/** How wide a viewport has to be before lanes run across it. */
export function flowFor(viewportWidth: number): Flow {
	return viewportWidth >= NARROW_WIDTH ? "across" : "down";
}

/** What a lane means, at the top level of a project. */
export const LANE_LABELS: Readonly<Record<Lane, string>> = {
	start: "start — once, when the game opens",
	step: "step — every frame, before painting",
	paint: "paint — every frame, to the screen",
};

/** One node's measured size, in pixels. */
export interface Size {
	readonly width: number;
	readonly height: number;
}

/** One node, placed. */
export interface PlacedNode extends Size {
	readonly id: NodeId;
	readonly x: number;
	readonly y: number;
}

/** One lane's band. */
export interface PlacedLane extends Size {
	readonly lane: Lane;
	readonly label: string;
	readonly x: number;
	readonly y: number;
}

/** One wire, as an SVG path between two ports. */
export interface PlacedWire {
	readonly id: WireId;
	readonly kind: WireKind;
	/** The `d` attribute of the path. */
	readonly path: string;
	/** Where the two ends sit, for the dots drawn on them. */
	readonly from: Point;
	readonly to: Point;
}

/** A point in board pixels. */
export interface Point {
	readonly x: number;
	readonly y: number;
}

/** A whole board, placed. */
export interface Layout {
	readonly flow: Flow;
	/** How much room the board needs, so the surface can be scrolled over it. */
	readonly width: number;
	readonly height: number;
	readonly lanes: readonly PlacedLane[];
	readonly nodes: readonly PlacedNode[];
	readonly wires: readonly PlacedWire[];
}

/** How much of a board is shown. */
export type Detail = "simple" | "pro";

/** What to lay out, and how. */
export interface LayoutOptions {
	readonly flow: Flow;
	/** What each lane's strip says. Varies with the board you are standing in. */
	readonly labels?: Readonly<Record<Lane, string>>;
	/**
	 * How much to show. One board at two detail levels, never two boards.
	 *
	 * Simple leaves out the data wires — at Simple a board reads as *what
	 * happens, in order*, and the data wires are the answer to a question
	 * nobody has asked yet. The nodes, the lanes and the execution order are
	 * the same either way, which is what makes this a level rather than a
	 * second editor.
	 */
	readonly detail?: Detail;
}

/**
 * Places every lane, node and wire.
 *
 * A node with no measured size is skipped rather than placed at a guess — a
 * node drawn at a size nobody measured is the failure this whole module is
 * arranged to avoid, and dropping it is at least visible.
 */
export function layout(
	board: Board,
	kinds: Registry,
	sizes: ReadonlyMap<NodeId, Size>,
	options: LayoutOptions,
): Layout {
	const labels = options.labels ?? LANE_LABELS;
	const across = options.flow === "across";
	// One width per column across every lane, so the columns line up and a
	// data wire between two lanes runs straight rather than ragged.
	const columns = columnWidths(board, sizes);
	const contentWidth = across
		? sum(columns) + NODE_GAP * Math.max(0, columns.length - 1)
		: Math.max(0, ...columns);

	const lanes: PlacedLane[] = [];
	const nodes: PlacedNode[] = [];
	let y = PAD;

	for (const lane of LANES) {
		const held = board.nodes
			.filter((node) => node.lane === lane && sizes.has(node.id))
			.sort((a, b) => a.column - b.column);
		if (held.length === 0) {
			continue;
		}
		const top = y + LABEL_HEIGHT;
		let depth = 0;
		for (const node of held) {
			const size = sizes.get(node.id) as Size;
			const x = across
				? PAD + sum(columns.slice(0, node.column)) + NODE_GAP * node.column
				: PAD;
			// Across, every node in a lane starts on the same line and `depth` is
			// the tallest of them. Down, `depth` is how far the stack has got.
			const y = across ? top : top + depth;
			nodes.push({ id: node.id, x, y, ...size });
			depth = across
				? Math.max(depth, size.height)
				: depth + size.height + NODE_GAP;
		}
		// A lane is as tall as what is in it. Sized for the widest node in an
		// across flow and for the whole stack in a down one, which is why this
		// cannot be a constant: Simple mode hides the port lists, and a lane
		// sized for Pro leaves a hole on a phone where the space is dearest.
		const filled = across ? depth : depth - NODE_GAP;
		const height = LABEL_HEIGHT + filled + PAD;
		lanes.push({
			lane,
			label: labels[lane],
			x: PAD / 2,
			y,
			width: contentWidth + PAD,
			height,
		});
		y += height + LANE_GAP;
	}

	const placed = new Map(nodes.map((node) => [node.id, node]));
	const wires: PlacedWire[] = [];
	const detail = options.detail ?? "pro";
	for (const wire of board.wires) {
		if (detail === "simple" && wire.kind === "data") {
			continue;
		}
		const from = anchor(board, kinds, placed, wire.from, "out", options.flow);
		const to = anchor(board, kinds, placed, wire.to, "in", options.flow);
		if (from === undefined || to === undefined) {
			continue;
		}
		wires.push({
			id: wire.id,
			kind: wire.kind,
			path: bow(from, to, across),
			from,
			to,
		});
	}

	return {
		flow: options.flow,
		width: contentWidth + PAD * 2,
		height: y - LANE_GAP + PAD,
		lanes,
		nodes,
		wires,
	};
}

/** The widest node in each column, across every lane. */
function columnWidths(
	board: Board,
	sizes: ReadonlyMap<NodeId, Size>,
): number[] {
	const widths: number[] = [];
	for (const node of board.nodes) {
		const size = sizes.get(node.id);
		if (size === undefined) {
			continue;
		}
		widths[node.column] = Math.max(widths[node.column] ?? 0, size.width);
	}
	// A column nobody occupies still takes a slot, so the columns after it do
	// not slide left and leave the lanes disagreeing about where column 2 is.
	return Array.from(widths, (width) => width ?? 0);
}

/**
 * Where a wire attaches, in board pixels.
 *
 * Ports are spread evenly along the node's leading or trailing edge, which
 * edge depending on the flow — that is what makes a wire re-route when the
 * lanes turn a corner rather than pointing at where the node used to be. The
 * execution port comes first on both sides, so the exec chain runs along one
 * consistent line through a lane.
 */
function anchor(
	board: Board,
	kinds: Registry,
	placed: ReadonlyMap<NodeId, PlacedNode>,
	end: { node: NodeId; port: PortName },
	side: "in" | "out",
	flow: Flow,
): Point | undefined {
	const node = placed.get(end.node);
	const held = board.nodes.find((candidate) => candidate.id === end.node);
	if (node === undefined || held === undefined) {
		return undefined;
	}
	const order = ports(kinds, held, side);
	const index = order.indexOf(end.port);
	if (index < 0) {
		return undefined;
	}
	const fraction = (index + 1) / (order.length + 1);
	if (flow === "across") {
		return {
			x: side === "out" ? node.x + node.width : node.x,
			y: node.y + node.height * fraction,
		};
	}
	return {
		x: node.x + node.width * fraction,
		y: side === "out" ? node.y + node.height : node.y,
	};
}

/**
 * The ports on one side of a node, in the order they are laid along its edge.
 *
 * The same list at either detail level. What Simple hides is the port *list*
 * printed in the box and the data wires between them, never where a port is —
 * so the execution wires land in exactly the same place at both levels, and
 * switching detail does not move the board under you.
 */
export function ports(
	kinds: Registry,
	node: BoardNode,
	side: "in" | "out",
): PortName[] {
	const kind = kindOf(kinds, node.kind);
	const data = side === "in" ? (kind?.inputs ?? []) : (kind?.outputs ?? []);
	return [side === "in" ? EXEC_IN : EXEC_OUT, ...data.map((port) => port.name)];
}

/**
 * A curve from one port to another, bowing along whichever axis it travels.
 *
 * Not simply along the direction of flow. A data wire between two lanes leaves
 * a trailing edge and arrives at a leading edge that is directly below it, and
 * bowing sideways there throws a lasso out past the lane before coming back —
 * legible, but it reads as a wire going somewhere it is not. Bowing along the
 * dominant axis keeps a wire inside the shape of the board.
 */
function bow(from: Point, to: Point, across: boolean): string {
	const forward = to.x - from.x;
	const dy = Math.abs(to.y - from.y);
	// Sideways only when the wire genuinely travels *forward* along x. A wire
	// that goes backwards — a data wire from a node in one lane to a node in
	// the same column of the next — would otherwise be thrown out past the
	// lane and dragged back, which reads as going somewhere it is not.
	const sideways =
		forward > 0 && (across ? forward * 2 >= dy : forward >= dy * 2);
	if (sideways) {
		const reach = Math.max(WIRE_BOW, forward / 2);
		return `M ${from.x} ${from.y} C ${from.x + reach} ${from.y}, ${to.x - reach} ${to.y}, ${to.x} ${to.y}`;
	}
	const reach = Math.max(WIRE_BOW, dy / 2);
	return `M ${from.x} ${from.y} C ${from.x} ${from.y + reach}, ${to.x} ${to.y - reach}, ${to.x} ${to.y}`;
}

/** Adds up a list of numbers. */
function sum(values: readonly number[]): number {
	return values.reduce((total, value) => total + value, 0);
}
