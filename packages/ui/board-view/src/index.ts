// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * The board view: a document, drawn.
 *
 * **Tier: `ui`.** The first thing in it, and the tier exists for exactly this
 * — the editor's chrome is HTML, per #812, and it is not the engine's
 * business to draw. It reaches down to `@krudd/board` for what a board *is*
 * and no further; the board is the source of truth and this is one way of
 * looking at it, never the other way round.
 *
 * Read-only. Editing is a later PR, and keeping the two apart means the thing
 * that draws a board is never also the thing that changes one.
 *
 * - `layout` — where everything goes, as arithmetic over measured sizes. No
 *   DOM, so the geometry can be asserted under `node:test`.
 * - `nav` — where you are in a project, and how you got there. Also no DOM.
 * - `view` — the elements, the measuring, panning, and the breadcrumb.
 *
 * The stylesheet is a separate export rather than injected: a page decides
 * what it loads, and a package that wrote to `document.head` on import would
 * be a package that cannot be rendered anywhere else.
 */

export type {
	Detail,
	Flow,
	Layout,
	LayoutOptions,
	PlacedLane,
	PlacedNode,
	PlacedWire,
	Point,
	Size,
} from "./layout";
export { flowFor, LANE_LABELS, layout, NARROW_WIDTH, ports } from "./layout";
export type { Crumb } from "./nav";
export { Trail } from "./nav";
export type { BoardView, BoardViewOptions } from "./view";
export { mountBoardView } from "./view";
