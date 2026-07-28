// SPDX-License-Identifier: GPL-2.0-or-later
//
// The engine's live state, as a React hook.

import { useEffect, useRef, useState } from "react";

import { bootEngine } from "./boot.js";
import type { EngineInfo, EngineStatus } from "./types.js";

const UNBUILT: EngineStatus = { phase: "unbuilt", renderer: null, error: null };
const LOADING: EngineStatus = { phase: "loading", renderer: null, error: null };

/**
 * Boot `engine` into the returned canvas ref and track what it reports.
 *
 * The effect has no cleanup that tears the module down, because there is none
 * to have — see boot.ts. It removes the hooks it installed and leaves the
 * engine running, which is correct for a page that boots one engine and keeps
 * it for its lifetime.
 */
export function useEngine(engine: EngineInfo): {
	status: EngineStatus;
	canvasRef: React.RefObject<HTMLCanvasElement | null>;
	log: readonly string[];
} {
	const canvasRef = useRef<HTMLCanvasElement | null>(null);
	const [status, setStatus] = useState<EngineStatus>(
		engine.built ? LOADING : UNBUILT
	);
	const [log, setLog] = useState<readonly string[]>([]);

	useEffect(() => {
		const canvas = canvasRef.current;
		if (!engine.built || canvas === null) return;

		const handle = bootEngine({
			engine,
			canvas,
			onStatus: setStatus,
			/* Bounded: a module that logs in its main loop would otherwise grow
			 * this array without limit. The console panel that replaces this is
			 * #954's, and it gets to decide its own retention. */
			onLog: (line) =>
				setLog((prior) => [...prior, line].slice(-LOG_LIMIT)),
		});

		return () => handle.dispose();
	}, [engine]);

	return { status, canvasRef, log };
}

const LOG_LIMIT = 200;
