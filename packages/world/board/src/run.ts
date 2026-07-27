// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * Running a board.
 *
 * The demo stops being code here. `Start` runs once when the board opens,
 * `Step` and `Paint` once each per frame, and what happens in each is
 * whatever the document's wires say — not what this file says.
 *
 * ## What runs, and what does not
 *
 * **The root board runs.** A nested board is viewable rather than runnable: a
 * node that carries one still executes its own `run`, and the board behind it
 * describes what that does. The frame board is the case in point — opening
 * `Draw Entities` shows clear → camera → draw → present, which is the frame
 * the engine already builds, not a second implementation of it.
 *
 * So the kinds a nested board is made of have no `run` at all, and that is
 * the distinction this file has to keep honest: a kind with no implementation
 * on a board that *does* run is refused by name when the runner is built.
 * "This board did nothing" must never look like "this board ran".
 *
 * ## Order
 *
 * A lane's order is its execution wires, walked from the entry point. The
 * document already guarantees the walk is a walk rather than a search:
 * validation refuses a node that two things run, a node that runs two things,
 * and any ring. So there is exactly one chain per lane and no ambiguity for
 * this file to resolve by picking.
 */

import type {
	Board,
	BoardNode,
	Lane,
	NodeId,
	NodeKind,
	ParamValue,
	Project,
	Registry,
} from "./document";
import { paramOf } from "./document";
import { KINDS, kindOf } from "./kinds";
import { BoardError, type Problem, validate } from "./validate";
import type { RunContext, WorldView } from "./world";

/** One node, resolved: what it is, and what it was set to. */
interface Step {
	readonly node: BoardNode;
	readonly kind: NodeKind;
	readonly params: Readonly<Record<string, ParamValue>>;
}

/**
 * A document, running against a world.
 *
 * Built once and reused every frame: resolving kinds and walking the wires is
 * work that does not change between frames, and doing it per frame would put
 * a map lookup per node into the hot path for nothing. When a board is edited
 * the chains are rebuilt — which is what makes an edit reach the running game
 * on the next frame rather than on a reload.
 */
export class Runner {
	readonly #world: WorldView;
	readonly #project: Project;
	readonly #kinds: Registry;
	/** The resolved chain for each lane, in the order it runs. */
	#lanes: Map<Lane, Step[]>;
	#frames = 0;
	#elapsed = 0;

	/**
	 * Resolves a document against a registry and a world.
	 *
	 * Throws a [`BoardError`] if the document does not hold together or names
	 * a kind that cannot run, rather than starting and discovering it a frame
	 * at a time.
	 */
	constructor(project: Project, world: WorldView, kinds: Registry = KINDS) {
		const problems = validate(project, kinds);
		if (problems.length > 0) {
			throw new BoardError(problems);
		}
		this.#project = project;
		this.#world = world;
		this.#kinds = kinds;
		this.#lanes = this.#resolve();
	}

	/** How many frames have run. */
	get frameCount(): number {
		return this.#frames;
	}

	/** Seconds of simulated time since `start`. */
	get elapsed(): number {
		return this.#elapsed;
	}

	/** The nodes one lane runs, in order — for a test, or a readout. */
	order(lane: Lane): NodeId[] {
		return (this.#lanes.get(lane) ?? []).map((step) => step.node.id);
	}

	/**
	 * Runs the start lane. Once, when the board opens.
	 *
	 * Before it does, every column the root board declares is made to exist —
	 * one `ensureColumn` per declared column, never per node — so a kind's
	 * `run` can resolve a column-name param straight into a live column
	 * without first asking whether it has been created yet.
	 */
	start(): void {
		for (const column of this.#rootBoard().columns ?? []) {
			this.#world.ensureColumn(column.name, column.kind);
		}
		this.#runLane("start", 0);
	}

	/** Runs the step lane and then the paint lane. Once per frame. */
	frame(dt: number): void {
		this.#runLane("step", dt);
		const painted = this.#runLane("paint", dt);
		if (!painted) {
			// A board with nothing left in its paint lane has to leave the
			// screen showing nothing. Without this the last frame it drew stays
			// on the canvas, and a cut wire reads as a frozen game rather than a
			// blank one — "working" and "did nothing", told the same way.
			this.#world.presentCleared();
		}
		this.#frames += 1;
		this.#elapsed += dt;
	}

	/**
	 * Re-reads the board after an edit.
	 *
	 * The document is held by reference, so an edit to it is already visible;
	 * this is what turns the edit into the chains the next frame walks. Kept
	 * explicit rather than re-resolving every frame so that an invalid
	 * intermediate state — a wire half-dragged — is a decision the editor
	 * makes about when to commit, not a crash mid-frame.
	 */
	reload(): void {
		this.#lanes = this.#resolve();
	}

	/** Walks the wires and resolves every kind, for every lane. */
	#resolve(): Map<Lane, Step[]> {
		const board = this.#rootBoard();
		const nodes = new Map(board.nodes.map((node) => [node.id, node]));
		const next = new Map<NodeId, NodeId>();
		for (const wire of board.wires) {
			// A cut wire is still in the document and is simply not followed.
			// That is what makes cutting one reversible, and what makes the
			// nodes past it read as inactive rather than as missing.
			if (wire.kind === "exec" && wire.cut !== true) {
				next.set(wire.from.node, wire.to.node);
			}
		}

		const lanes = new Map<Lane, Step[]>();
		const problems: Problem[] = [];
		for (const node of board.nodes) {
			const kind = kindOf(this.#kinds, node.kind);
			if (kind?.entry === undefined) {
				continue;
			}
			const steps: Step[] = [];
			// From the node *after* the entry point: an entry point is where
			// execution begins, not something that runs.
			let at = next.get(node.id);
			while (at !== undefined) {
				const held = nodes.get(at);
				const heldKind =
					held === undefined ? undefined : kindOf(this.#kinds, held.kind);
				if (held === undefined || heldKind === undefined) {
					break;
				}
				if (heldKind.run === undefined) {
					problems.push({
						board: this.#project.root,
						node: held.id,
						message: `\`${held.kind}\` has no implementation, so this board cannot run — it can only be looked at`,
					});
					break;
				}
				steps.push({
					node: held,
					kind: heldKind,
					params: settings(held, heldKind),
				});
				at = next.get(at);
			}
			lanes.set(kind.entry, steps);
		}

		if (problems.length > 0) {
			throw new BoardError(problems);
		}
		return lanes;
	}

	/** The board the project opens on, which validation has already found. */
	#rootBoard(): Board {
		const board = this.#project.boards[this.#project.root];
		if (board === undefined) {
			throw new BoardError([
				{
					message: `the project opens on board \`${this.#project.root}\`, which it does not hold`,
				},
			]);
		}
		return board;
	}

	/** Runs one lane's chain, in order. Reports whether it ran anything. */
	#runLane(lane: Lane, dt: number): boolean {
		const steps = this.#lanes.get(lane);
		if (steps === undefined || steps.length === 0) {
			return false;
		}
		for (const step of steps) {
			// Not `?.`: a step only reaches this list once its kind has been
			// found to have an implementation.
			(step.kind.run as NonNullable<NodeKind["run"]>)(
				context(this.#world, dt, step),
			);
		}
		return true;
	}
}

/** Every param a node runs with: its own, or its kind's default. */
function settings(
	node: BoardNode,
	kind: NodeKind,
): Readonly<Record<string, ParamValue>> {
	const params: Record<string, ParamValue> = {};
	for (const declared of kind.params) {
		const value = paramOf(node, kind, declared.name);
		if (value !== undefined) {
			params[declared.name] = value;
		}
	}
	return params;
}

/** What one node is handed. */
function context(world: WorldView, dt: number, step: Step): RunContext {
	const read = (name: string): ParamValue => {
		const value = step.params[name];
		if (value === undefined) {
			// Validation refuses a param the kind does not declare, so a kind
			// asking for one it did not declare is a bug here rather than in the
			// document — and a silent zero would present as an entity that did
			// not move.
			throw new Error(
				`node \`${step.node.id}\` asked for param \`${name}\`, which \`${step.node.kind}\` does not declare`,
			);
		}
		return value;
	};
	return {
		world,
		dt,
		number(name: string): number {
			const value = read(name);
			if (typeof value !== "number") {
				throw new Error(`param \`${name}\` is a name, not a number`);
			}
			return value;
		},
		text(name: string): string {
			const value = read(name);
			if (typeof value !== "string") {
				throw new Error(`param \`${name}\` is a number, not a name`);
			}
			return value;
		},
	};
}
