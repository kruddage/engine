// SPDX-License-Identifier: GPL-2.0-or-later
//
// fps and resolution for the status strip, measured from the page.
//
// ## Why these are not read from the engine
//
// `editor_layout.scm` declares `fps`, `resolution` and `driver` as status-bar
// fields "the host updates by id each frame", and the generated shell renders
// them with `id="status-fps"` and friends so a host can. Nothing in the tree
// actually writes them — they are a contract with a writer that does not exist
// yet, and have been since #706.
//
// #954 is explicitly not allowed to add one: no new engine exports, no new
// EM_JS, no boundary work. So the fields are sourced from what the page can
// honestly know on its own, which turns out to be all three:
//
// - **fps** — the engine drives its main loop through
//   `emscripten_set_main_loop`, which is `requestAnimationFrame`. Counting
//   frames here counts the same frames, so this is the engine's rate rather
//   than an approximation of it. It stops being true the moment the engine
//   runs its loop off anything else, and that is worth knowing about.
// - **resolution** — the engine sets the canvas's physical size every tick
//   (`kruddgui.cpp` measures the CSS box and writes `width`/`height`). Reading
//   those attributes back is reading what the engine decided, not guessing.
// - **driver** — the backend that reported through `kruddSetRenderer`. That is
//   a real push channel and it already works.
//
// When #945's boundary carries engine stats, this gets re-pointed at it and
// nothing above it changes. That re-pointing is #948's, and it is why the hook
// returns a shape rather than rendering anything.

import { useEffect, useState } from "react";

export interface FrameStats {
	/** Frames per second over the last sampling window, or null before one. */
	fps: number | null;
	/** The canvas's physical size, or null while there is no canvas. */
	resolution: { width: number; height: number } | null;
}

const EMPTY: FrameStats = { fps: null, resolution: null };

/** How long a sample runs before fps is republished. Long enough not to jitter. */
const WINDOW_MS = 500;

/**
 * Track the page's frame rate and the canvas's physical size.
 *
 * `canvas` may be null — the hook simply reports nothing until there is one,
 * which is the state during boot and the state forever on an unbuilt engine.
 */
export function useFrameStats(canvas: HTMLCanvasElement | null): FrameStats {
	const [stats, setStats] = useState<FrameStats>(EMPTY);

	useEffect(() => {
		if (typeof requestAnimationFrame !== "function") return;

		let running = true;
		let frames = 0;
		let since = now();
		let handle = 0;

		const tick = (): void => {
			if (!running) return;
			frames += 1;
			const elapsed = now() - since;
			if (elapsed >= WINDOW_MS) {
				const fps = Math.round((frames * 1000) / elapsed);
				frames = 0;
				since = now();
				setStats((prior) =>
					prior.fps === fps ? prior : { ...prior, fps }
				);
			}
			handle = requestAnimationFrame(tick);
		};

		handle = requestAnimationFrame(tick);
		return () => {
			running = false;
			cancelAnimationFrame(handle);
		};
	}, []);

	useEffect(() => {
		if (canvas === null) {
			setStats((prior) =>
				prior.resolution === null ? prior : { ...prior, resolution: null }
			);
			return;
		}

		const read = (): void => {
			const next = { width: canvas.width, height: canvas.height };
			setStats((prior) =>
				prior.resolution?.width === next.width &&
				prior.resolution?.height === next.height
					? prior
					: { ...prior, resolution: next }
			);
		};

		read();

		/*
		 * A ResizeObserver watches the CSS box, which is what changes first;
		 * the engine writes the physical size in response, on its next tick.
		 * Reading in the observer therefore reports one frame late, which is
		 * correct — reporting the size the engine has not adopted yet would be
		 * wrong, not fresher.
		 *
		 * Absent in jsdom, and absent is fine: the initial read above still
		 * reports the size, it just stops tracking. A component suite is not
		 * where a resize is worth proving.
		 */
		if (typeof ResizeObserver !== "function") return;
		const observer = new ResizeObserver(read);
		observer.observe(canvas);
		return () => observer.disconnect();
	}, [canvas]);

	return stats;
}

function now(): number {
	return typeof performance === "object" && performance
		? performance.now()
		: Date.now();
}

/**
 * The driver a backend name stands for.
 *
 * The same mapping the generated shell uses, kept identical on purpose: two
 * pages driving one engine must not disagree about what to call the thing that
 * is drawing.
 */
export function driverLabel(renderer: string | null): string {
	if (renderer === null) return "—";
	if (renderer === "webgpu") return "WebGPU";
	if (renderer === "webgl") return "WebGL";
	return renderer;
}
