// SPDX-License-Identifier: GPL-2.0-or-later
//
// Booting the engine module from the editor's own page.
//
// The engine is one emscripten link unit whose loader is a classic script
// reading a global `Module` — that is what `{{{ SCRIPT }}}` expands to in the
// generated shell, and it is the interface we have. So this file does what the
// shell does: define the global, inject the loader, and hand the module a
// canvas. Nothing here is a boundary, and none of it should be mistaken for one
// (#945 builds that; two exported functions and a push-only EM_JS call is what
// exists today).
//
// ## locateFile, and the hash that is deliberately absent
//
// The deployed site renames index.wasm to index.<hash>.wasm and the generated
// shell resolves it through a locateFile hook built from the same commit. Two
// derivations of one hash in two languages that have to agree — @kruddage/site
// cross-checks them for exactly that reason.
//
// The editor sidesteps the whole mechanism: the plugin copies the artifacts
// under their real names, so locateFile here is the identity. That is not an
// oversight and it must not "get fixed" by copying the shell's version — a
// stem applied to files that were never renamed asks the browser for a URL that
// was never staged, and it fails at runtime and nowhere else.
//
// ## The callbacks
//
// The engine's EM_JS bridges are written defensively — every one of them checks
// `typeof window.x === 'function'` or wraps the call in try/catch — so a host
// page implements the ones it wants and the rest are no-ops. We take the four
// that report boot state and the renderer, and deliberately leave the rest
// alone: kruddBuildEditor in particular is the old Scheme chrome's entry point,
// and answering it would be the editor growing a second shell (#953 retires it).

import type { EngineInfo, EngineStatus } from "./types.js";

/** The subset of the emscripten module object this page sets up. */
interface EmscriptenModule {
	canvas: HTMLCanvasElement;
	locateFile: (path: string, prefix: string) => string;
	print: (text: string) => void;
	printErr: (text: string) => void;
	onAbort: (what: unknown) => void;
}

/** The engine's host hooks, as its EM_JS bridges look for them. */
interface KruddHostHooks {
	Module?: EmscriptenModule;
	kruddSetRunning?: () => void;
	kruddSetReady?: () => void;
	kruddSetRenderer?: (name: string) => void;
	kruddSetRendererFailed?: (name: string, why: string) => void;
	kruddBootGame?: () => string;
}

type HostWindow = Window & KruddHostHooks;

export interface BootOptions {
	engine: EngineInfo;
	canvas: HTMLCanvasElement;
	/** Called on every state change, with the whole status each time. */
	onStatus: (status: EngineStatus) => void;
	/** Called for each line the module writes to stdout or stderr. */
	onLog?: (line: string, stream: "out" | "err") => void;
}

/** A handle that stops the boot's effects on the page. */
export interface BootHandle {
	dispose: () => void;
}

const LOADER = "index.js";

/**
 * Boot the engine into `canvas`, reporting progress through `onStatus`.
 *
 * Returns a handle whose dispose() removes the injected script and the host
 * hooks. It does not tear the module down — emscripten offers no honest way to
 * do that for a module that has started its main loop, and pretending otherwise
 * would leave a half-dead engine driving a detached canvas. React strict mode's
 * double-mount is handled by refusing to boot twice (see hasBooted below)
 * rather than by unwinding something that cannot be unwound.
 */
export function bootEngine(options: BootOptions): BootHandle {
	const { engine, canvas, onStatus, onLog } = options;

	if (!engine.built) {
		onStatus({ phase: "unbuilt", renderer: null, error: null });
		return { dispose: () => {} };
	}

	const host = window as HostWindow;

	if (hasBooted) {
		/* Already running from an earlier mount. Report what we know rather than
		 * injecting a second copy of a module that owns a canvas and a main
		 * loop. */
		onStatus(lastStatus);
		return { dispose: () => {} };
	}
	hasBooted = true;

	const update = (next: Partial<EngineStatus>): void => {
		lastStatus = { ...lastStatus, ...next };
		onStatus(lastStatus);
	};

	update({ phase: "loading", renderer: null, error: null });

	host.Module = {
		canvas,
		/* Identity. See the note at the top of this file before changing it. */
		locateFile: (path, prefix) => prefix + path,
		print: (text) => onLog?.(text, "out"),
		printErr: (text) => onLog?.(text, "err"),
		onAbort: (what) => update({ phase: "failed", error: String(what) }),
	};

	host.kruddSetRunning = () => update({ phase: "running" });
	host.kruddSetReady = () => update({ phase: "ready" });
	host.kruddSetRenderer = (name) => update({ renderer: name });
	host.kruddSetRendererFailed = (name, why) =>
		update({ phase: "failed", renderer: name, error: why });

	/* "none" leaves the launcher standing instead of auto-loading chess. The
	 * editor opens a project; it does not start playing one. */
	host.kruddBootGame = () => "none";

	const script = document.createElement("script");
	script.async = true;
	script.src = engine.base + LOADER;
	script.addEventListener("error", () => {
		update({
			phase: "failed",
			error: `failed to load ${script.src} — the engine may not be built (${engine.buildCommand})`,
		});
	});
	document.body.appendChild(script);

	return {
		dispose: () => {
			script.remove();
			delete host.kruddSetRunning;
			delete host.kruddSetReady;
			delete host.kruddSetRenderer;
			delete host.kruddSetRendererFailed;
			delete host.kruddBootGame;
		},
	};
}

/* Module-scoped rather than per-call: the guard has to survive a component
 * remount, which is the case it exists for. */
let hasBooted = false;
let lastStatus: EngineStatus = {
	phase: "loading",
	renderer: null,
	error: null,
};

/**
 * Forget that a boot happened.
 *
 * For tests only. Nothing in the app calls this — a page boots the engine once.
 */
export function resetBootStateForTests(): void {
	hasBooted = false;
	lastStatus = { phase: "loading", renderer: null, error: null };
}
