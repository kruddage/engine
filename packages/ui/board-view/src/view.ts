// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * The board, drawn.
 *
 * Read-only: this shows a document and lets you move around it. Editing is a
 * later PR, and keeping the two apart means the thing that draws a board is
 * never also the thing that changes one.
 *
 * ## Measure, then place
 *
 * Nodes go into the document first, at their natural height, and are measured
 * before anything is positioned. `layout.ts` then decides where everything
 * sits from those measurements. The order is the whole trick: a node box is as
 * tall as its own content, and a guessed height spills the text past the
 * border *and* lands every wire off its port, because the ports are placed as
 * fractions of the box.
 *
 * ## HTML for the boxes, SVG for the wires
 *
 * Boxes are HTML because text in HTML wraps, hyphenates and honours the
 * reader's font size, and because #812 settled that the editor is HTML. Wires
 * are one SVG overlay because a curve is a curve. The two share a coordinate
 * space — the surface both sit inside — so a wire lands on a port without
 * either half converting anything.
 */

import type { Board, BoardId, Lane, NodeId, Registry } from "@krudd/board";
import { KINDS, kindOf, paramOf } from "@krudd/board";
import { type Flow, flowFor, layout, ports, type Size } from "./layout";

/** The SVG namespace, which `createElement` does not imply. */
const SVG_NS = "http://www.w3.org/2000/svg";

/** A mounted board view. */
export interface BoardView {
	/** Draws a board. Measures first, so it must be called while visible. */
	show(board: Board, labels?: Readonly<Record<Lane, string>>): void;
	/** Re-measures and re-places, after a resize or a change of detail. */
	refresh(): void;
	/** Stops listening. */
	destroy(): void;
}

/** How to mount one. */
export interface BoardViewOptions {
	/** Where it goes. Emptied on mount. */
	readonly host: HTMLElement;
	/** The kinds the document's nodes are resolved through. */
	readonly kinds?: Registry;
	/** Called when a node carrying a board is opened. */
	readonly onOpen?: (board: BoardId, node: NodeId) => void;
}

/**
 * Builds a board view inside `host`.
 *
 * The view owns two layers and a pan offset, and nothing else. It does not own
 * the document: `show` is handed one, and the caller keeps it.
 */
export function mountBoardView(options: BoardViewOptions): BoardView {
	const kinds = options.kinds ?? KINDS;
	const host = options.host;
	host.textContent = "";
	host.classList.add("board-view");

	const surface = document.createElement("div");
	surface.className = "board-surface";
	const wires = document.createElementNS(SVG_NS, "svg");
	wires.setAttribute("class", "board-wires");
	const boxes = document.createElement("div");
	boxes.className = "board-nodes";
	surface.append(wires, boxes);
	host.append(surface);

	let board: Board | null = null;
	let labels: Readonly<Record<Lane, string>> | undefined;
	let pan = { x: 0, y: 0 };

	const place = (): void => {
		if (board === null) {
			return;
		}
		// The flow first, and only then the measuring. It is on the host because
		// the stylesheet keys off it, so setting it afterwards would measure
		// boxes at one width and place them at another — which is the same
		// guessed-size failure as never measuring at all, arrived at the long
		// way round.
		const flow = flowFor(host.clientWidth);
		host.dataset.flow = flow;

		// Measured now, from what the browser actually laid out — see the
		// module docs for why this cannot be a table of constants.
		const sizes = new Map<NodeId, Size>();
		for (const element of boxes.querySelectorAll<HTMLElement>(".board-node")) {
			const id = element.dataset.node;
			if (id !== undefined) {
				sizes.set(id, {
					width: element.offsetWidth,
					height: element.offsetHeight,
				});
			}
		}

		const placed = layout(board, kinds, sizes, {
			flow,
			...(labels === undefined ? {} : { labels }),
		});

		surface.style.width = `${placed.width}px`;
		surface.style.height = `${placed.height}px`;
		wires.setAttribute("width", String(placed.width));
		wires.setAttribute("height", String(placed.height));
		wires.setAttribute("viewBox", `0 0 ${placed.width} ${placed.height}`);

		drawLanes(boxes, placed.lanes);
		for (const node of placed.nodes) {
			const element = boxes.querySelector<HTMLElement>(
				`.board-node[data-node="${cssEscape(node.id)}"]`,
			);
			if (element !== null) {
				element.style.transform = `translate(${node.x}px, ${node.y}px)`;
			}
		}
		drawWires(wires, placed.wires);
		applyPan();
	};

	const applyPan = (): void => {
		surface.style.translate = `${pan.x}px ${pan.y}px`;
	};

	/** Pan by drag. Interior only — the edges belong to the mode shell. */
	let drag: { pointer: number; x: number; y: number } | null = null;
	const onDown = (event: PointerEvent): void => {
		drag = { pointer: event.pointerId, x: event.clientX, y: event.clientY };
	};
	const onMove = (event: PointerEvent): void => {
		if (drag === null || drag.pointer !== event.pointerId) {
			return;
		}
		pan = {
			x: pan.x + (event.clientX - drag.x),
			y: pan.y + (event.clientY - drag.y),
		};
		drag = { pointer: event.pointerId, x: event.clientX, y: event.clientY };
		applyPan();
	};
	const onUp = (event: PointerEvent): void => {
		if (drag?.pointer === event.pointerId) {
			drag = null;
		}
	};
	host.addEventListener("pointerdown", onDown);
	host.addEventListener("pointermove", onMove);
	host.addEventListener("pointerup", onUp);
	host.addEventListener("pointercancel", onUp);

	// A resize can cross the width where lanes stop running across and start
	// stacking down, so it is a re-place rather than only a repaint.
	const onResize = (): void => place();
	window.addEventListener("resize", onResize);

	return {
		show(next: Board, nextLabels?: Readonly<Record<Lane, string>>): void {
			board = next;
			labels = nextLabels;
			// Opened at the top-left with a margin rather than zoomed to fit: a
			// board scaled down to fit a phone is a board nobody can read, and
			// the first thing anyone wants to see is where it starts.
			pan = { x: 0, y: 0 };
			buildNodes(boxes, next, kinds, options.onOpen);
			place();
		},
		refresh: place,
		destroy(): void {
			host.removeEventListener("pointerdown", onDown);
			host.removeEventListener("pointermove", onMove);
			host.removeEventListener("pointerup", onUp);
			host.removeEventListener("pointercancel", onUp);
			window.removeEventListener("resize", onResize);
		},
	};
}

/** Builds one box per node, unpositioned, so they can be measured. */
function buildNodes(
	host: HTMLElement,
	board: Board,
	kinds: Registry,
	onOpen: BoardViewOptions["onOpen"],
): void {
	for (const stale of host.querySelectorAll(".board-node")) {
		stale.remove();
	}
	for (const node of board.nodes) {
		const kind = kindOf(kinds, node.kind);
		const box = document.createElement("article");
		box.className = "board-node";
		box.dataset.node = node.id;
		box.dataset.lane = node.lane;
		if (kind?.entry !== undefined) {
			box.dataset.entry = "true";
		}

		const title = document.createElement("h3");
		title.textContent = kind?.title ?? node.kind;
		box.append(title);

		for (const declared of kind?.params ?? []) {
			const row = document.createElement("p");
			row.className = "board-param";
			const name = document.createElement("span");
			name.textContent = declared.name;
			const value = document.createElement("b");
			value.textContent = String(
				paramOf(node, kind ?? emptyKind(), declared.name),
			);
			row.append(name, value);
			box.append(row);
		}

		for (const side of ["in", "out"] as const) {
			// The execution port is on every node and is drawn as the chain
			// rather than as a name, so it is not listed here.
			const named = ports(kinds, node, side).slice(1);
			if (named.length === 0) {
				continue;
			}
			const row = document.createElement("p");
			row.className = "board-ports";
			row.dataset.side = side;
			row.textContent = `${side} ${named.join(", ")}`;
			box.append(row);
		}

		if (node.board !== undefined) {
			const open = document.createElement("p");
			open.className = "board-opens";
			open.textContent = "opens a board";
			box.append(open);
			if (onOpen !== undefined) {
				const target = node.board;
				box.addEventListener("click", () => onOpen(target, node.id));
			}
		}
		host.append(box);
	}
}

/** Draws the lane bands and their labels. */
function drawLanes(
	host: HTMLElement,
	lanes: readonly {
		lane: Lane;
		label: string;
		x: number;
		y: number;
		width: number;
		height: number;
	}[],
): void {
	for (const stale of host.querySelectorAll(".board-lane")) {
		stale.remove();
	}
	for (const lane of lanes) {
		const band = document.createElement("div");
		band.className = "board-lane";
		band.dataset.lane = lane.lane;
		band.style.transform = `translate(${lane.x}px, ${lane.y}px)`;
		band.style.width = `${lane.width}px`;
		band.style.height = `${lane.height}px`;
		const label = document.createElement("span");
		label.textContent = lane.label;
		band.append(label);
		// Before the nodes in document order so the band sits behind them
		// without either needing a z-index to argue about.
		host.prepend(band);
	}
}

/** Draws every wire, and a dot on each end. */
function drawWires(
	host: SVGElement,
	wires: readonly {
		id: string;
		kind: string;
		path: string;
		from: { x: number; y: number };
		to: { x: number; y: number };
	}[],
): void {
	host.textContent = "";
	for (const wire of wires) {
		const group = document.createElementNS(SVG_NS, "g");
		group.setAttribute("class", "board-wire");
		// Execution order and data are told apart by more than colour: the
		// data wire is dashed as well, because roughly one man in twelve cannot
		// be relied on to see the difference between coral and teal.
		group.setAttribute("data-kind", wire.kind);
		const path = document.createElementNS(SVG_NS, "path");
		path.setAttribute("d", wire.path);
		group.append(path);
		for (const end of [wire.from, wire.to]) {
			const dot = document.createElementNS(SVG_NS, "circle");
			dot.setAttribute("cx", String(end.x));
			dot.setAttribute("cy", String(end.y));
			dot.setAttribute("r", "3.5");
			group.append(dot);
		}
		host.append(group);
	}
}

/** A kind with nothing in it, for a node whose kind did not resolve. */
function emptyKind(): NonNullable<ReturnType<typeof kindOf>> {
	return { title: "", inputs: [], outputs: [], params: [] };
}

/**
 * A node id, safe inside an attribute selector.
 *
 * Ids come from a document, and a document is untrusted input — an id holding
 * a quote would otherwise end the selector and start something else.
 */
function cssEscape(value: string): string {
	return CSS.escape(value);
}

export type { Flow };
