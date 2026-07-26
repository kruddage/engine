// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * The board, drawn.
 *
 * Shows a document, lets you move around it, and lets you change it — cutting
 * a wire and setting a param. Every edit goes through `@krudd/board`'s edit
 * operations rather than reaching into the document here, so there is one
 * place that knows how a document may be changed.
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

import type { Board, BoardId, NodeId, Project, Registry } from "@krudd/board";
import {
	drivesTheGame,
	KINDS,
	kindOf,
	paramOf,
	setParam,
	toggleWire,
} from "@krudd/board";
import {
	type Detail,
	type Flow,
	flowFor,
	layout,
	type PlacedLane,
	type PlacedWire,
	ports,
	reachable,
	type Size,
} from "./layout";
import { Trail } from "./nav";

/** The SVG namespace, which `createElement` does not imply. */
const SVG_NS = "http://www.w3.org/2000/svg";

/** A mounted board view. */
export interface BoardView {
	/** Opens a project at its root board. Measures, so it must be visible. */
	open(project: Project): void;
	/** Re-measures and re-places, after a resize or a change of detail. */
	refresh(): void;
	/** How much is shown. Simple leaves out the data wires and the port lists. */
	detail: Detail;
	/** Which board is showing, root-first. */
	readonly path: readonly BoardId[];
	/** Stops listening. */
	destroy(): void;
}

/** How to mount one. */
export interface BoardViewOptions {
	/** Where it goes. Emptied on mount. */
	readonly host: HTMLElement;
	/** The kinds the document's nodes are resolved through. */
	readonly kinds?: Registry;
	/** Where it starts. Simple, unless a page says otherwise. */
	readonly detail?: Detail;
	/**
	 * Called after any edit to the document.
	 *
	 * The board is the source of truth, so an edit here *is* an edit to the
	 * project — this is only how whatever is running it finds out, so the
	 * change reaches the game on the next frame rather than on a reload.
	 */
	readonly onEdit?: () => void;
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

	// The breadcrumb and the detail toggle sit outside the surface, because
	// the surface is what pans — chrome that slid off the top when somebody
	// dragged the board would be chrome nobody could get back to.
	const bar = document.createElement("nav");
	bar.className = "board-bar";
	const crumbs = document.createElement("div");
	crumbs.className = "board-crumbs";
	const toggle = document.createElement("button");
	toggle.type = "button";
	toggle.className = "board-detail";
	bar.append(crumbs, toggle);
	host.append(bar);

	// The settings sheet rolls up from the bottom. Outside the surface for the
	// same reason the bar is: it must not pan away with the board.
	const sheet = document.createElement("aside");
	sheet.className = "board-sheet";
	sheet.hidden = true;
	host.append(sheet);

	const surface = document.createElement("div");
	surface.className = "board-surface";
	const wires = document.createElementNS(SVG_NS, "svg");
	wires.setAttribute("class", "board-wires");
	const boxes = document.createElement("div");
	boxes.className = "board-nodes";
	surface.append(wires, boxes);
	host.append(surface);

	let trail: Trail | null = null;
	let detail: Detail = options.detail ?? "simple";
	let pan = { x: 0, y: 0 };

	const place = (): void => {
		const board = trail?.board;
		if (trail === null || board === undefined) {
			return;
		}
		host.dataset.detail = detail;
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
			detail,
			labels: trail.labels,
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
		const held = trail;
		drawWires(wires, placed.wires, (id) =>
			drivesTheGame(held.project, held.here, id, kinds),
		);
		// Everything the cut stopped reads as stopped. A board where only the
		// wire changed would leave the consequence for you to work out.
		const live = reachable(board, kinds);
		for (const element of boxes.querySelectorAll<HTMLElement>(".board-node")) {
			const id = element.dataset.node;
			element.dataset.inactive =
				id !== undefined && !live.has(id) ? "true" : "false";
		}
		drawBar(crumbs, toggle, trail, detail);
		applyPan();
	};

	/** Redraws the boxes and then places them. */
	const draw = (): void => {
		const board = trail?.board;
		if (trail === null || board === undefined) {
			return;
		}
		// Back to the top-left on every change of board: you have arrived
		// somewhere new, and the first thing anyone wants to see is where it
		// starts.
		pan = { x: 0, y: 0 };
		buildNodes(boxes, board, kinds, detail, canOpen);
		place();
	};

	/** Whether a node's board can be descended into at this detail level. */
	const canOpen = (id: BoardId): boolean => {
		const target = trail?.project.boards[id];
		return target !== undefined && (detail === "pro" || target.pro !== true);
	};

	const setDetail = (next: Detail): void => {
		detail = next;
		// Simple does not reach into a Pro board, so standing in one when the
		// level changes means climbing out rather than looking at something
		// Simple says is not there.
		trail?.settle(next);
		draw();
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
	bar.addEventListener("click", (event) => {
		const target = event.target;
		if (target === toggle) {
			setDetail(detail === "simple" ? "pro" : "simple");
			return;
		}
		const crumb = target instanceof Element ? target.closest("button") : null;
		const id = crumb?.dataset.board;
		if (id !== undefined && trail?.goTo(id) === true) {
			draw();
		}
	});

	boxes.addEventListener("click", (event) => {
		const target = event.target;
		const box =
			target instanceof Element ? target.closest(".board-node") : null;
		if (!(box instanceof HTMLElement) || trail === null) {
			return;
		}
		const opens = box.dataset.opens;
		if (opens !== undefined && trail.enter(opens, detail)) {
			draw();
			return;
		}
		const node = box.dataset.node;
		if (node !== undefined) {
			openSheet(node);
		}
	});

	// Tapping a wire cuts it; tapping again reconnects it. Only wires that
	// genuinely drive the running game are listening — see `drawWires`.
	wires.addEventListener("click", (event) => {
		const target = event.target;
		const group = target instanceof Element ? target.closest("g") : null;
		const id = group?.getAttribute("data-wire");
		const held = trail;
		if (id === null || id === undefined || held === null) {
			return;
		}
		if (toggleWire(held.project, held.here, id) !== undefined) {
			place();
			options.onEdit?.();
		}
	});

	/**
	 * Rolls the settings sheet up for one node.
	 *
	 * Editing a value edits the document, and the document is what is running —
	 * so the change reaches the game on the next frame, with no reload.
	 */
	const openSheet = (id: NodeId): void => {
		const held = trail;
		const board = held?.board;
		const node = board?.nodes.find((candidate) => candidate.id === id);
		const kind = node === undefined ? undefined : kindOf(kinds, node.kind);
		if (held === null || node === undefined || kind === undefined) {
			return;
		}
		sheet.textContent = "";
		sheet.hidden = false;

		const head = document.createElement("header");
		const title = document.createElement("h3");
		title.textContent = kind.title;
		const close = document.createElement("button");
		close.type = "button";
		close.className = "board-sheet-close";
		close.textContent = "done";
		close.addEventListener("click", () => {
			sheet.hidden = true;
		});
		head.append(title, close);
		sheet.append(head);

		if (kind.params.length === 0) {
			const empty = document.createElement("p");
			empty.className = "board-sheet-empty";
			empty.textContent = "nothing to set";
			sheet.append(empty);
			return;
		}

		for (const declared of kind.params) {
			const row = document.createElement("label");
			row.className = "board-sheet-row";
			const name = document.createElement("span");
			name.textContent = declared.name;
			const input = document.createElement("input");
			const numeric = declared.type === "f32" || declared.type === "u32";
			input.type = numeric ? "number" : "text";
			if (numeric) {
				// The kind's own bounds, so the control cannot produce a value
				// `validate` would then refuse.
				input.step = declared.type === "u32" ? "1" : "any";
				if (declared.min !== undefined) {
					input.min = String(declared.min);
				}
				if (declared.max !== undefined) {
					input.max = String(declared.max);
				}
			}
			input.value = String(paramOf(node, kind, declared.name) ?? "");
			input.addEventListener("input", () => {
				const value = numeric ? Number(input.value) : input.value;
				if (numeric && !Number.isFinite(value as number)) {
					return;
				}
				if (
					setParam(held.project, held.here, id, declared.name, value, kinds)
				) {
					draw();
					options.onEdit?.();
				}
			});
			row.append(name, input);
			sheet.append(row);
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
		open(project: Project): void {
			// Opened at the top-left with a margin rather than zoomed to fit: a
			// board scaled down to fit a phone is a board nobody can read, and
			// the first thing anyone wants to see is where it starts.
			trail = new Trail(project);
			draw();
		},
		refresh: place,
		get detail(): Detail {
			return detail;
		},
		set detail(next: Detail) {
			setDetail(next);
		},
		get path(): readonly BoardId[] {
			return trail?.crumbs.map((crumb) => crumb.id) ?? [];
		},
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
	detail: Detail,
	canOpen: (board: BoardId) => boolean,
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

		// Port lists are Pro. At Simple a board reads as what happens and in
		// what order, and a port type is the answer to a question nobody
		// standing at Simple has asked yet.
		if (detail === "pro") {
			for (const side of ["in", "out"] as const) {
				// The execution port is on every node and is drawn as the chain
				// rather than as a name, so it is not listed here.
				const named = ports(kinds, node, side)
					.slice(1)
					.map((name) => withType(kinds, node, side, name));
				if (named.length === 0) {
					continue;
				}
				const row = document.createElement("p");
				row.className = "board-ports";
				row.dataset.side = side;
				row.textContent = `${side} ${named.join(", ")}`;
				box.append(row);
			}
		}

		if (node.board !== undefined && canOpen(node.board)) {
			box.dataset.opens = node.board;
			const open = document.createElement("p");
			open.className = "board-opens";
			open.textContent = "open ›";
			box.append(open);
		}
		host.append(box);
	}
}

/** A port with its type, which is the other thing Pro adds. */
function withType(
	kinds: Registry,
	node: { kind: string },
	side: "in" | "out",
	name: string,
): string {
	const kind = kindOf(kinds, node.kind);
	const port = (side === "in" ? kind?.inputs : kind?.outputs)?.find(
		(candidate) => candidate.name === name,
	);
	return port === undefined ? name : `${name}: ${port.type}`;
}

/**
 * Draws the breadcrumb and the detail toggle.
 *
 * The breadcrumb is buttons rather than text because every crumb but the last
 * is a way back, and a way back that does not look like a control is a way
 * back nobody finds.
 */
function drawBar(
	crumbs: HTMLElement,
	toggle: HTMLButtonElement,
	trail: Trail,
	detail: Detail,
): void {
	crumbs.textContent = "";
	const path = trail.crumbs;
	for (const [index, crumb] of path.entries()) {
		if (index > 0) {
			const chevron = document.createElement("span");
			chevron.textContent = "›";
			crumbs.append(chevron);
		}
		const button = document.createElement("button");
		button.type = "button";
		button.dataset.board = crumb.id;
		button.textContent = crumb.title;
		// The board you are standing in is not a way back to itself.
		button.disabled = index === path.length - 1;
		button.setAttribute("aria-current", button.disabled ? "true" : "false");
		crumbs.append(button);
	}
	// Scrolled to the end rather than reversed: a path too long for a phone
	// should run off the left, and the crumb worth seeing is the one you are
	// standing in — but the order it reads in is root first, always.
	crumbs.scrollLeft = crumbs.scrollWidth;
	toggle.textContent = detail === "simple" ? "simple" : "pro";
	toggle.dataset.detail = detail;
	toggle.setAttribute(
		"aria-label",
		detail === "simple"
			? "showing simple, tap for pro"
			: "showing pro, tap for simple",
	);
}

/** Draws the lane bands and their labels. */
function drawLanes(host: HTMLElement, lanes: readonly PlacedLane[]): void {
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
	wires: readonly PlacedWire[],
	cuttable: (id: string) => boolean,
): void {
	host.textContent = "";
	for (const wire of wires) {
		const group = document.createElementNS(SVG_NS, "g");
		group.setAttribute("class", "board-wire");
		group.setAttribute("data-wire", wire.id);
		group.setAttribute("data-cut", wire.cut ? "true" : "false");
		// Only wires that genuinely drive the running game take a tap. A
		// control that looks live and changes nothing teaches the wrong thing
		// about what the board is.
		if (cuttable(wire.id)) {
			group.setAttribute("data-cuttable", "true");
			// A stroke wide enough for a thumb, invisible, under the drawn one.
			const grab = document.createElementNS(SVG_NS, "path");
			grab.setAttribute("class", "board-wire-grab");
			grab.setAttribute("d", wire.path);
			group.append(grab);
		}
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
