// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * The benchmark behind the batching rule.
 *
 * "The boundary is batched, never per-object" is the load-bearing claim of
 * the architecture, and until this existed it was an assertion. It is now a
 * number: the same world, read and written the batched way and the per-call
 * way, timed, with the ratio printed.
 *
 * It is also a correctness check. Both paths of a pair accumulate a checksum
 * as they go and the run fails if the two disagree — a benchmark whose fast
 * path quietly skipped the work would report a magnificent speedup, which is
 * the "instrument that cannot fail loudly" failure applied to timing.
 *
 * Run by `cargo xtask bench`. See `docs/boundary.md` for what it measures and
 * what the numbers mean.
 */

import type { Engine } from "../generated/krudd_web.js";
import type { World } from "../src/index";
import { load, populate } from "./load";

/** How many entities the timed world holds. */
const ENTITIES = 20_000;

/** How many frames each path is timed over. */
const FRAMES = 20;

/** Frames run before timing starts, to let the JIT settle. */
const WARMUP_FRAMES = 5;

/** The last slot, read back each frame to prove the writes landed. */
const LAST = ENTITIES - 1;

/**
 * The floor the batched path must clear.
 *
 * Set far below what the two paths actually measure — the point is to catch
 * the batched path silently becoming per-object, not to police a ratio. A
 * tight bound here would be a flaky gate, and a flaky gate is one people
 * learn to re-run rather than read.
 */
const MIN_SPEEDUP = 2;

/** One timed path. */
interface Measurement {
	/** What it did, for the table. */
	readonly label: string;
	/** Wall clock across `FRAMES` frames, in milliseconds. */
	readonly ms: number;
	/** The value the other half of the pair has to agree on. */
	readonly checksum: number;
}

/** Times one frame function over a warmup and then a measured run. */
function measure(label: string, frame: (n: number) => number): Measurement {
	for (let i = 0; i < WARMUP_FRAMES; i++) {
		frame(i);
	}
	let checksum = 0;
	const start = performance.now();
	for (let i = 0; i < FRAMES; i++) {
		checksum += frame(i);
	}
	return { label, ms: performance.now() - start, checksum };
}

/**
 * Reads every coordinate out of the view over Rust's own column.
 *
 * One call across the boundary — and the cached view makes even that free
 * when nothing moved — then a walk over a typed array.
 */
function readBatched(world: World): number {
	const positions = world.positions();
	let sum = 0;
	for (let i = 0; i < positions.length; i++) {
		sum += positions[i] as number;
	}
	return sum;
}

/**
 * Reads the same coordinates one boundary crossing at a time.
 *
 * The components are summed in the same order the batched walk visits them,
 * so the two checksums are bit-identical rather than merely close. An epsilon
 * comparison would hide a path that skipped an entity.
 */
function readPerCall(engine: Engine, count: number): number {
	let sum = 0;
	for (let slot = 0; slot < count; slot++) {
		const p = engine.position_of(slot);
		if (p === undefined) {
			continue;
		}
		sum += p[0] as number;
		sum += p[1] as number;
		sum += p[2] as number;
	}
	return sum;
}

/** Prints one row of the report. */
function row(m: Measurement, work: number, ratio: string): void {
	const perEntity = ((m.ms * 1e6) / work).toFixed(1);
	console.log(
		m.label.padEnd(28) +
			m.ms.toFixed(1).padStart(10) +
			perEntity.padStart(12) +
			ratio.padStart(13),
	);
}

/** Fails the run if a pair of paths did not do the same work. */
function verify(batched: Measurement, perCall: Measurement): void {
	if (batched.checksum !== perCall.checksum) {
		throw new Error(
			`"${batched.label}" and "${perCall.label}" disagree ` +
				`(${batched.checksum} vs ${perCall.checksum}) — they are not doing ` +
				"the same work, so the ratio between them means nothing",
		);
	}
}

/** Fails the run if the batched path has stopped being the fast one. */
function demand(what: string, ratio: number): void {
	if (ratio < MIN_SPEEDUP) {
		throw new Error(
			`the batched ${what} path is only ${ratio.toFixed(1)}× faster than the ` +
				`per-call one, under the ${MIN_SPEEDUP}× floor this asserts. Either ` +
				"the batched path has become per-object, or the contract in " +
				"docs/boundary.md needs rewriting around the new numbers.",
		);
	}
}

/** Runs both pairs and reports. */
async function main(): Promise<void> {
	const { world, engine } = await load(1920, 1080);
	populate(world, ENTITIES);
	// One tick so the column holds something other than the spawn positions;
	// the read paths are then timed against a world that is standing still, so
	// both see the same bytes and their checksums are comparable.
	world.tick(1 / 60);

	const batchedRead = measure("read · batched view", () => readBatched(world));
	const perCallRead = measure("read · one call per entity", () =>
		readPerCall(engine, ENTITIES),
	);

	// Writes. The batched path is not a call at all — TypeScript writes into
	// Rust's column through the view, and Rust reads it on the next tick.
	const batchedWrite = measure("write · into the view", (n) => {
		const positions = world.positions();
		let sum = 0;
		for (let slot = 0; slot < ENTITIES; slot++) {
			const v = slot + n;
			const i = slot * 3;
			positions[i] = v;
			positions[i + 1] = -v;
			positions[i + 2] = 0;
			sum += v;
		}
		// Read one coordinate back, so a path that wrote nothing at all cannot
		// checksum the same as one that wrote everything.
		return sum + (positions[LAST * 3] as number);
	});
	const perCallWrite = measure("write · one call per entity", (n) => {
		let sum = 0;
		for (let slot = 0; slot < ENTITIES; slot++) {
			const v = slot + n;
			engine.set_position(slot, v, -v, 0);
			sum += v;
		}
		return sum + (engine.position_of(LAST)?.[0] ?? Number.NaN);
	});

	const work = ENTITIES * FRAMES;
	const readRatio = perCallRead.ms / batchedRead.ms;
	const writeRatio = perCallWrite.ms / batchedWrite.ms;

	console.log(`\nboundary benchmark — ${ENTITIES} entities × ${FRAMES} frames`);
	console.log(
		`\n${"path".padEnd(28)}${"total ms".padStart(10)}${"ns/entity".padStart(12)}${"vs batched".padStart(13)}`,
	);
	row(batchedRead, work, "1.0×");
	row(perCallRead, work, `${readRatio.toFixed(1)}×`);
	row(batchedWrite, work, "1.0×");
	row(perCallWrite, work, `${writeRatio.toFixed(1)}×`);

	verify(batchedRead, perCallRead);
	verify(batchedWrite, perCallWrite);
	demand("read", readRatio);
	demand("write", writeRatio);

	console.log(
		`\nbatched is ${readRatio.toFixed(0)}× cheaper to read through and ` +
			`${writeRatio.toFixed(0)}× cheaper to write through, on this machine`,
	);
}

await main();
