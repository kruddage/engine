// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * Where you are in a project, and how you got there.
 *
 * Boards nest, so "which board am I looking at" is a path rather than an id —
 * and the path is what the breadcrumb walks back up. Kept apart from the DOM
 * for the same reason the geometry is: descending into a board and coming back
 * out is a rule, and a rule with an off-by-one in it looks like a breadcrumb
 * that drops a level or refuses to come home.
 *
 * ## Detail is not depth
 *
 * Simple and Pro are one board at two levels, not two editors. What Simple
 * leaves out is the data wires and the port lists — the nodes, the lanes and
 * the execution order are the same — and, because some boards *are* the
 * detail, a board marked `pro` does not open at Simple at all. The frame is
 * the case in point: an eight-year-old placing a Draw Entities node has no
 * business being dropped into clear → camera → draw → present.
 */

import type { Board, BoardId, Lane, Project } from "@krudd/board";
import type { Detail } from "./layout";
import { LANE_LABELS } from "./layout";

/** One step of the path: which board, and what it is called. */
export interface Crumb {
	readonly id: BoardId;
	readonly title: string;
}

/**
 * A path into a project, from the root board down.
 *
 * Never empty: there is always a board you are standing in, and the root is
 * the floor.
 */
export class Trail {
	readonly #project: Project;
	#path: BoardId[];

	/** Starts at the board the project opens on. */
	constructor(project: Project) {
		this.#project = project;
		this.#path = [project.root];
	}

	/** The project this walks. */
	get project(): Project {
		return this.#project;
	}

	/** Where you are. */
	get here(): BoardId {
		// Never empty — `up` refuses to leave the root.
		return this.#path[this.#path.length - 1] as BoardId;
	}

	/** How deep, counting the root as zero. */
	get depth(): number {
		return this.#path.length - 1;
	}

	/** The board you are standing in. */
	get board(): Board | undefined {
		return board(this.#project, this.here);
	}

	/** The path, for the breadcrumb to draw. */
	get crumbs(): Crumb[] {
		return this.#path.map((id) => ({
			id,
			title: board(this.#project, id)?.title ?? id,
		}));
	}

	/**
	 * Descends into a board. Returns whether it went anywhere.
	 *
	 * Refuses a board the project does not hold, one that is already on the
	 * path — which would be a breadcrumb with no top — and, at Simple, one that
	 * is a Pro concept.
	 */
	enter(id: BoardId, detail: Detail): boolean {
		const target = board(this.#project, id);
		if (target === undefined || this.#path.includes(id)) {
			return false;
		}
		if (detail === "simple" && target.pro === true) {
			return false;
		}
		this.#path.push(id);
		return true;
	}

	/** Comes back up one. Returns whether it went anywhere. */
	up(): boolean {
		if (this.#path.length <= 1) {
			return false;
		}
		this.#path.pop();
		return true;
	}

	/**
	 * Goes back to a board already on the path, as tapping a crumb does.
	 *
	 * Returns whether it went anywhere. A crumb that is not on the path is not
	 * a crumb, so it is refused rather than treated as a descent.
	 */
	goTo(id: BoardId): boolean {
		const at = this.#path.indexOf(id);
		if (at < 0 || at === this.#path.length - 1) {
			return false;
		}
		this.#path = this.#path.slice(0, at + 1);
		return true;
	}

	/**
	 * Climbs out of anywhere the detail level no longer reaches.
	 *
	 * Switching to Simple while standing inside a Pro board would otherwise
	 * leave you looking at something Simple says does not exist. Returns
	 * whether it had to move.
	 */
	settle(detail: Detail): boolean {
		if (detail !== "simple") {
			return false;
		}
		let moved = false;
		while (this.depth > 0 && board(this.#project, this.here)?.pro === true) {
			this.#path.pop();
			moved = true;
		}
		return moved;
	}

	/** What the lanes of the board you are standing in mean. */
	get labels(): Readonly<Record<Lane, string>> {
		return this.board?.lanes ?? LANE_LABELS;
	}
}

/**
 * One board by id.
 *
 * Through `Object.hasOwn`, because an id comes from a document and a plain
 * object indexed by one answers for `constructor` as readily as for a board.
 */
function board(project: Project, id: BoardId): Board | undefined {
	return Object.hasOwn(project.boards, id) ? project.boards[id] : undefined;
}
