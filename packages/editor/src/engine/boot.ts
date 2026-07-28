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
// page implements the ones it wants and the rest are no-ops. We take the ones
// that report boot state and the renderer, and deliberately leave the rest
// alone: kruddBuildEditor in particular is the old Scheme chrome's entry point,
// and answering it would be the editor growing a second shell (#953 retires it).
//
// ## kruddWantsWebGPU is not optional, whatever the guard says
//
// krudd_wants_webgpu (engine.c) is the one bridge whose fallback is a decision
// rather than a no-op: its catch returns 1, so a host that does not implement it
// selects WebGPU unconditionally. On a machine with no adapter that is the worst
// available answer, and it is not hypothetical — it is what a headless CI
// browser is.
//
// So the editor answers it, mirroring the shell's contract exactly (Firefox
// forced to WebGL, ?renderer=webgl as the opt-out, WebGPU otherwise). The shell
// calls itself the single source of truth for "should this page run WebGPU" so
// its chrome and the backend never disagree; the editor needs the same property
// for the same reason, and having a second page silently disagree with it about
// the default would be worse than either choice.

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
	kruddWantsWebGPU?: () => boolean;
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
	/**
	 * Which backend to ask for. Defaults to the shell's own rule — see
	 * wantsWebGPU below. Passed explicitly only by tests.
	 */
	wantsWebGPU?: boolean;
}

/**
 * Whether this page should run WebGPU, by the shell's rule.
 *
 * Deliberately a copy of `window.kruddWantsWebGPU` in the generated shell
 * rather than an improvement on it: Firefox is forced to WebGL until its
 * implementation is further along, `?renderer=webgl` is the opt-out, and
 * WebGPU is the default everywhere else. Two host pages driving one engine
 * must not disagree about this.
 *
 * Note what it does *not* do: probe `navigator.gpu`. Presence of the object
 * says nothing about whether an adapter will be handed over, so a probe would
 * swap one wrong answer for a subtler one. The engine already reports
 * "this browser returned no GPU adapter" through kruddSetRendererFailed, and
 * that report is the honest signal.
 */
export function wantsWebGPU(search: string = window.location.search): boolean {
	try {
		if (/firefox/i.test(navigator.userAgent)) return false;
		return new URLSearchParams(search).get("renderer") !== "webgl";
	} catch {
		return true;
	}
}

/** A handle that stops the boot's effects on the page. */
export interface BootHandle {
	dispose: () => void;
}

const LOADER = "index.js";

/**
 * The id the engine's canvas must carry.
 *
 * **This is a hard contract with the C tree, not a convention.** The engine does
 * not take a canvas handle — it looks the element up by selector, from ten call
 * sites that all hardcode `"#canvas"`:
 *
 * - `render/webgl/renderer_webgl.c` creates the GL context against it
 * - `ui/kruddgui/kruddgui.cpp` registers every mouse, touch and wheel callback
 *   on it, measures its CSS size, and sets its physical size each tick
 *
 * A host page that renames it gets no GL context *and* no input, and the
 * failure is not reported anywhere near the cause:
 * `emscripten_webgl_create_context` returns 0, the return is not checked, the
 * subsystem logs "renderer_webgl: init" as though it succeeded, and the first
 * `glCreateShader` later dies with "Cannot read properties of undefined". That
 * is exactly how this cost three CI runs to find.
 *
 * `Module.canvas` is set as well, but it is not a substitute — it is what
 * emscripten's own runtime uses, while the selector is what the engine uses.
 * Both have to point at the same element.
 *
 * #954's viewport panel must keep this id when it takes the canvas over.
 */
export const ENGINE_CANVAS_ID = "canvas";

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

	/* Answered rather than left to the engine's catch — see the note above. */
	const preferWebGPU = options.wantsWebGPU ?? wantsWebGPU();
	host.kruddWantsWebGPU = () => preferWebGPU;

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
			delete host.kruddWantsWebGPU;
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
