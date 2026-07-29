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
// ## Where the loader comes from
//
// `engine.base` is **relative to the page** (`ENGINE_DIR`, engine-artifacts.mts)
// and is joined to `document.baseURI` here. It has to be relative for the same
// reason vite.config.ts sets `base: "./"`: one build is served at the site root,
// at a sub-route and under a PR-number prefix, and none of those tell it its own
// URL.
//
// It was absolute, and that was invisible for exactly as long as every harness
// served the editor at "/" — which the dev server and `vite preview` both did.
// GitHub Pages serves this site under `/engine/`, where "/engine/index.js" is
// not the page's own directory but the *Pages root*. Worse than a 404: the
// repository is also called `engine`, so the request landed on the site root,
// where the deploy's `keep_files` leaves stale bundles from old deploys. A 200
// carrying the wrong JavaScript raises no `error` event, defines no `Module`,
// and fires no `onRuntimeInitialized` — so the page sat on "loading…" with an
// empty console, a black viewport and both panels saying "Waiting for the
// engine". Nothing failed, so nothing was reported.
//
// Note what the join below does and does not buy. It makes the injected `src`
// an absolute URL, which is what a reader debugging this wants to see — but a
// relative `src` would resolve identically, and a base that began with "/"
// would escape to the origin root through `new URL` just as it did through
// string concatenation. **The property that fixes this is the base being
// relative, not the join**, so that is what is pinned: build's own suite asserts
// the shape (engine-artifacts.test.ts) and the browser suite now serves the
// build under a prefix rather than at "/" (playwright.config.ts). A jsdom test
// cannot catch it, because jsdom resolves a relative src the same way.
//
// ## The callbacks
//
// The engine's EM_JS bridges are written defensively — every one of them checks
// `typeof window.x === 'function'` or wraps the call in try/catch — so a host
// page implements the ones it wants and the rest are no-ops. We take the ones
// that report boot state and the renderer, and deliberately leave the rest
// alone. kruddBuildEditor was the one that mattered — the old Scheme chrome's
// entry point, which answering would have made this editor grow a second shell.
// #953 retired it on the engine side, so there is nothing left to decline.
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
//
// ## What the editor boots into
//
// Chess, by the shell's rule — see bootGame below.
//
// This answered "none" unconditionally until now, on the reasoning that the
// editor opens a project rather than starting to play one. The reasoning is
// still right and the answer was still wrong, because of what "none" actually
// leaves on *this* page. In the generated shell it leaves the launcher
// standing, which is a real choice offered to a reader. Here there is no
// launcher to leave standing: the overlay is DOM the shell owns — `#launcher`
// and `#launcher-games` live in shell.html.in, and `game_launcher_add`
// (game/host/game.c) is written to no-op when it cannot find them, so a host
// page without the overlay gets no buttons rather than a crash. The editor's
// "none" therefore did not mean "pick a scene"; it meant "no scene, and
// nothing offering one", and what a reader got on first load was the demo
// scene seed_demo_scene leaves behind and no way to reach a game from inside
// the editor at all.
//
// A boot scene is a placeholder for the project this editor cannot open yet.
// A cold start has nothing in the project store (document/project-store.ts)
// until the reader has saved something, so File > Open has nothing to offer on
// a fresh page either — leaving the reader with no route to a populated scene
// from any direction. Landing on chess gives the outliner, the inspector and
// the gizmo something to be about, which is the whole reason those panels are
// worth looking at before the editor can open assets of its own.
//
// **This is temporary, and the shape of its removal is known.** When the editor
// opens real projects, the boot scene goes back to being the reader's choice
// and this returns to "none". `?game=none` reaches that state today, so the
// previous behaviour is one query parameter away rather than gone.

import type { BridgeModule } from "@kruddage/engine/bridge";

import type { EngineInfo, EngineStatus } from "./types.js";

/** The subset of the emscripten module object this page sets up. */
interface EmscriptenModule {
	canvas: HTMLCanvasElement;
	locateFile: (path: string, prefix: string) => string;
	print: (text: string) => void;
	printErr: (text: string) => void;
	onAbort: (what: unknown) => void;
	onRuntimeInitialized: () => void;
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
	 * Called once the wasm runtime is up, with the module object itself.
	 *
	 * This is the only way anything reaches the boundary: createBridge needs
	 * HEAPU8, UTF8ToString and the four `_krudd_bridge_*` exports, and they
	 * exist on the module rather than on `window`. It fires at
	 * onRuntimeInitialized rather than at kruddSetReady, because the exports
	 * are callable as soon as the runtime is, and waiting for the engine to
	 * finish booting its subsystems would mean the first flush is later than
	 * it needs to be for no reason.
	 *
	 * Not the same event as `phase: "ready"`. The runtime being up says the
	 * module can be called; ready says the engine finished starting.
	 */
	onModule?: (module: BridgeModule) => void;
	/**
	 * Which backend to ask for. Defaults to the shell's own rule — see
	 * wantsWebGPU below. Passed explicitly only by tests.
	 */
	wantsWebGPU?: boolean;
	/**
	 * Which scene to boot into. Defaults to the shell's own rule — see bootGame
	 * below. Passed explicitly only by tests.
	 */
	bootGame?: string;
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

/**
 * The scene this page boots into, by the shell's rule.
 *
 * Chess by default, `?game=<name>` for another registered game — matched
 * case-insensitively against the label it registered under (game_find in
 * game/host/game.c) — and `?game=none`, or any name no game registered under,
 * for no scene at all.
 *
 * Deliberately a copy of `window.kruddBootGame` in the generated shell rather
 * than an improvement on it, for the same reason wantsWebGPU is one: two host
 * pages driving one engine must not disagree about what the page is about to
 * show. The engine reads this once, after every game has registered and the
 * scene api is live (krudd_boot_game, core/engine.c), so there is no second
 * chance to answer it differently.
 *
 * Note what it does *not* do: check the name against anything. The registry
 * lives in the wasm module and is not populated when this is called, and an
 * unknown name already has a defined meaning — the launcher stands, which here
 * means no scene loads. A guess about which games exist would be a second copy
 * of that list, in the wrong language, free to drift.
 */
export function bootGame(search: string = window.location.search): string {
	try {
		return new URLSearchParams(search).get("game") ?? DEFAULT_GAME;
	} catch {
		return DEFAULT_GAME;
	}
}

const DEFAULT_GAME = "chess";

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
	const { engine, canvas, onStatus, onLog, onModule } = options;

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
		if (hostModule) onModule?.(hostModule);
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
		/*
		 * `host.Module` is the same object the loader augments in place —
		 * emscripten is linked without MODULARIZE, so the global this page
		 * defined *is* the module once the runtime is up. Reading it here
		 * rather than storing our own literal is what makes the exports
		 * and heap views visible.
		 */
		onRuntimeInitialized: () => {
			hostModule = host.Module as unknown as BridgeModule;
			onModule?.(hostModule);
		},
	};

	/* Answered rather than left to the engine's catch — see the note above. */
	const preferWebGPU = options.wantsWebGPU ?? wantsWebGPU();
	host.kruddWantsWebGPU = () => preferWebGPU;

	host.kruddSetRunning = () => update({ phase: "running" });
	host.kruddSetReady = () => update({ phase: "ready" });
	host.kruddSetRenderer = (name) => update({ renderer: name });
	host.kruddSetRendererFailed = (name, why) =>
		update({ phase: "failed", renderer: name, error: why });

	/*
	 * The scene to land on, answered rather than left to the engine's default
	 * for the same reason the backend is — the engine's own fallback is chess
	 * either way, but a host page that leaves it to a catch is a host page that
	 * cannot be told otherwise. See "What the editor boots into" above for why
	 * this is a scene at all, and why it is temporary.
	 */
	const game = options.bootGame ?? bootGame();
	host.kruddBootGame = () => game;

	const script = document.createElement("script");
	script.async = true;
	/* Joined to the page rather than concatenated into a path — see "Where the
	 * loader comes from" above, including what this does not fix. */
	script.src = new URL(engine.base + LOADER, document.baseURI).href;
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

/*
 * Module-scoped rather than per-call: the guard has to survive a component
 * remount, which is the case it exists for. `hostModule` is the same — a
 * second mount must be handed the module the first one booted, not left
 * waiting for an onRuntimeInitialized that already fired.
 */
let hasBooted = false;
let hostModule: BridgeModule | null = null;
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
	hostModule = null;
	lastStatus = { phase: "loading", renderer: null, error: null };
}
