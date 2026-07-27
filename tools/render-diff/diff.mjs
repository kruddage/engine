#!/usr/bin/env node
// SPDX-License-Identifier: GPL-2.0-or-later
//
// render-diff — screenshot oracle for the WebGPU port.
//
// Drives Chrome over the DevTools Protocol, captures the engine canvas under
// each backend, and compares. Zero dependencies: the CDP transport is Node's
// built-in WebSocket, and PNG decoding for the comparison happens inside the
// browser, which is already a PNG codec.
//
//   node tools/render-diff/diff.mjs --base <url> [--scene <id>] [--accept]
//
// See README.md for the mode ladder and why this must not run headless.

import { spawn, spawnSync } from 'node:child_process';
import { mkdtempSync, mkdirSync, readFileSync, writeFileSync, existsSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const SHOTS = join(HERE, 'shots');
const OUT = join(HERE, 'out');

/* Per-channel delta above which two pixels are "different". Absorbs the
 * sub-LSB wobble two GPU backends produce for identical maths. */
const CHANNEL_THRESHOLD = 8;

// ---------------------------------------------------------------- args

function parseArgs(argv) {
	const a = { base: 'http://127.0.0.1:8000', scene: null, accept: false, keepOpen: false };

	for (let i = 0; i < argv.length; i++) {
		switch (argv[i]) {
		case '--base':   a.base = argv[++i]; break;
		case '--scene':  a.scene = argv[++i]; break;
		case '--accept': a.accept = true; break;
		case '--help':   a.help = true; break;
		default:
			die(`unknown argument: ${argv[i]}`);
		}
	}
	return a;
}

function die(msg) {
	console.error(`render-diff: ${msg}`);
	process.exit(2);
}

const USAGE = `
render-diff — WebGPU vs WebGL screenshot oracle

  node tools/render-diff/diff.mjs [options]

  --base <url>    site root to test (default http://127.0.0.1:8000)
                  a PR preview works too, so no local emsdk is needed:
                  --base https://kruddage.github.io/engine/pr-preview/pr-123/
  --scene <id>    run one scene from scenes.json instead of all
  --accept        promote this run's output to the reference shots
  --help          this text

Must run on a real display. With DISPLAY unset it re-execs under xvfb-run.
`;

// ------------------------------------------------- the xvfb requirement

/*
 * Headless Chrome composites WebGPU canvases as blank — the official
 * webgpu-samples helloTriangle reproduces it, so it is a browser behaviour, not
 * an engine bug. WebGL captures fine either way, which is what makes it a trap:
 * the obvious sanity check confirms a broken instrument. There is deliberately
 * no --headless escape hatch; a mode that silently reports "nothing rendered"
 * is worse than no harness.
 */
function ensureDisplay(argv) {
	if (process.env.DISPLAY)
		return;

	const xvfb = spawnSync('sh', ['-c', 'command -v xvfb-run'], { encoding: 'utf8' });
	if (xvfb.status !== 0) {
		die('DISPLAY is unset and xvfb-run is not installed.\n' +
		    '  WebGPU canvases capture as blank in headless Chrome, so this\n' +
		    '  harness refuses to run without a display. Install xvfb, or run\n' +
		    '  under an existing X display.');
	}

	console.log('render-diff: no DISPLAY — re-execing under xvfb-run');
	const r = spawnSync('xvfb-run',
		['-a', '-s', '-screen 0 1400x900x24', process.execPath, fileURLToPath(import.meta.url), ...argv],
		{ stdio: 'inherit' });
	process.exit(r.status ?? 1);
}

// ------------------------------------------------------- CDP transport

/* Minimal DevTools Protocol client. One socket, id-matched replies, and a
 * listener list for events. */
class CDP {
	constructor(ws) {
		this.ws = ws;
		this.next = 1;
		this.pending = new Map();
		this.listeners = [];

		ws.addEventListener('message', (ev) => {
			const msg = JSON.parse(ev.data);
			if (msg.id && this.pending.has(msg.id)) {
				const { resolve, reject } = this.pending.get(msg.id);
				this.pending.delete(msg.id);
				msg.error ? reject(new Error(`${msg.error.message} (${JSON.stringify(msg.error.data ?? {})})`))
					  : resolve(msg.result);
				return;
			}
			for (const fn of this.listeners)
				fn(msg);
		});
	}

	static async connect(url) {
		const ws = new WebSocket(url);
		await new Promise((res, rej) => {
			ws.addEventListener('open', res, { once: true });
			ws.addEventListener('error', () => rej(new Error(`cannot connect to ${url}`)), { once: true });
		});
		return new CDP(ws);
	}

	send(method, params = {}, sessionId) {
		const id = this.next++;
		const msg = { id, method, params };
		if (sessionId)
			msg.sessionId = sessionId;
		this.ws.send(JSON.stringify(msg));
		return new Promise((resolve, reject) => this.pending.set(id, { resolve, reject }));
	}

	/* Returns an unsubscribe fn — every page and every wait removes its own
	 * listener, so they do not accumulate across scenes. */
	on(fn) {
		this.listeners.push(fn);
		return () => {
			const i = this.listeners.indexOf(fn);
			if (i >= 0)
				this.listeners.splice(i, 1);
		};
	}

	close() { this.ws.close(); }
}

// ------------------------------------------------------- chrome launch

async function launchChrome() {
	const profile = mkdtempSync(join(tmpdir(), 'render-diff-'));
	const bin = process.env.CHROME_BIN || '/usr/bin/google-chrome';

	const child = spawn(bin, [
		'--remote-debugging-port=0',
		`--user-data-dir=${profile}`,
		'--enable-unsafe-webgpu',
		'--enable-features=Vulkan',
		'--use-angle=vulkan',
		'--no-first-run',
		'--no-default-browser-check',
		'--disable-popup-blocking',
		/* A backgrounded or occluded tab gets its rendering throttled, which
		 * stops the engine's frame loop and captures the canvas black. Each
		 * scene opens a fresh tab, so without these the result is a coin
		 * flip between a correct capture and a uniformly black one. */
		'--disable-backgrounding-occluded-windows',
		'--disable-renderer-backgrounding',
		'--disable-background-timer-throttling',
		'--window-size=1400,900',
		'about:blank',
	], { stdio: ['ignore', 'ignore', 'pipe'] });

	/* Chrome announces the browser endpoint on stderr; port 0 means we must
	 * read it rather than assume 9222 and collide with a stray instance. */
	const wsUrl = await new Promise((resolve, reject) => {
		let buf = '';
		const timer = setTimeout(() => reject(new Error('chrome did not report a DevTools endpoint')), 20000);

		child.stderr.on('data', (chunk) => {
			buf += chunk.toString();
			const m = buf.match(/DevTools listening on (ws:\/\/\S+)/);
			if (m) {
				clearTimeout(timer);
				resolve(m[1]);
			}
		});
		child.on('exit', (code) => {
			clearTimeout(timer);
			reject(new Error(`chrome exited early (${code}); is ${bin} installed?`));
		});
	});

	return {
		cdp: await CDP.connect(wsUrl),
		kill: () => {
			child.kill();
			try { rmSync(profile, { recursive: true, force: true }); } catch { /* best effort */ }
		},
	};
}

// --------------------------------------------- WebGPU error capture shim

/*
 * The reason an agent porting this goes blind: a WebGPU validation failure
 * usually shows up as a blank canvas plus a message that never reaches the
 * page's console, so it is indistinguishable from "nothing was drawn". Wrap
 * requestDevice before any page script runs and forward uncaptured errors and
 * device-lost to console.error, where the driver already collects them.
 */
const GPU_ERROR_SHIM = `
(() => {
	if (typeof GPUAdapter === 'undefined') return;
	const TAG = '[gpu-error] ';
	const orig = GPUAdapter.prototype.requestDevice;
	if (!orig) return;
	GPUAdapter.prototype.requestDevice = async function (...args) {
		const dev = await orig.apply(this, args);
		try {
			dev.addEventListener('uncapturederror', (e) => {
				const err = e.error;
				console.error(TAG + (err && err.message ? err.message : String(err)));
			});
			dev.lost.then((info) => {
				console.error(TAG + 'device lost: ' + info.reason + ' — ' + info.message);
			});
		} catch (e) {
			console.error(TAG + 'shim failed: ' + e.message);
		}
		return dev;
	};
})();
`;

// ------------------------------------------------ deterministic capture clock

/*
 * The parity scene animates, and the engine drives that animation off its
 * frame dt, which comes from performance.now() (emscripten_get_now). Under a
 * wall-clock settle the two backends boot at different speeds and land on
 * different animation phases on every run — WebGPU boots slower, so it is
 * systematically behind — which is the ~16% *asymmetric* noise floor that
 * swamps real changes (#603).
 *
 * Replace real time, before any page script runs, with a virtual clock that
 * advances one fixed step per rendered animation frame and nothing else. Two
 * consequences make capture reproducible:
 *
 *   - Frame index, not wall-clock, is the phase. Frame N is the same animation
 *     phase on both backends no matter how long boot took, so the harness can
 *     wait for a fixed frame count instead of a fixed number of milliseconds.
 *   - The clock can be *held*. Once the harness has the frame it wants, it
 *     freezes the clock so the phase cannot drift while overlays are hidden and
 *     the screenshot (and any blank-capture retry) is taken.
 *
 * The capture window is anchored at first render: the shell assigns
 * window.kruddSetReady after this script runs, so we intercept the assignment
 * and record the frame it first fires on. Both backends run the same ready
 * logic under the same virtual clock, so any gap between "ready" and "the scene
 * animates" is the same number of frames on each and cancels out. Date.now() is
 * left real, so asset loading and cache-busting are untouched.
 */
const DETERMINISTIC_CLOCK = `
(() => {
	const STEP_MS = 1000 / 60;   // fixed per-frame dt
	let vt = 0;                  // virtual time (ms)
	let frame = 0;               // animation-frame index
	let lastReal = -1;           // real rAF timestamp of the current frame
	let readyFrame = -1;         // frame index kruddSetReady first fired on
	let held = false;            // frozen for capture

	/* Advance once per real animation frame. All rAF callbacks in one frame
	 * share a timestamp, so keying on it stops multiple rAF consumers (the
	 * engine loop, kruddgui, the DOM) from over-advancing the clock. */
	const realRAF = window.requestAnimationFrame.bind(window);
	window.requestAnimationFrame = (cb) => realRAF((realTs) => {
		if (!held && realTs !== lastReal) {
			lastReal = realTs;
			frame++;
			vt = frame * STEP_MS;
		}
		return cb(vt);
	});

	/* The engine reads performance.now() for its frame dt. The override is in
	 * place before the wasm runtime captures a reference to it. */
	performance.now = () => vt;

	let realReady = null;
	Object.defineProperty(window, 'kruddSetReady', {
		configurable: true,
		get() { return realReady; },
		set(fn) {
			realReady = function (...a) {
				if (readyFrame < 0) readyFrame = frame;
				return fn.apply(this, a);
			};
		},
	});

	window.__renderDiffClock = () => ({ frame, readyFrame, held });
	window.__renderDiffHold  = () => { held = true; return frame; };
})();
`;

// ------------------------------------------------------------ capturing

async function openPage(cdp) {
	const { targetId } = await cdp.send('Target.createTarget', { url: 'about:blank' });
	const { sessionId } = await cdp.send('Target.attachToTarget', { targetId, flatten: true });

	await cdp.send('Page.enable', {}, sessionId);
	await cdp.send('Runtime.enable', {}, sessionId);
	await cdp.send('Log.enable', {}, sessionId);
	/* Order matters only in that both run before page scripts: the clock
	 * installs its rAF/now overrides, the shim wraps requestDevice. */
	await cdp.send('Page.addScriptToEvaluateOnNewDocument', { source: DETERMINISTIC_CLOCK }, sessionId);
	await cdp.send('Page.addScriptToEvaluateOnNewDocument', { source: GPU_ERROR_SHIM }, sessionId);

	return { targetId, sessionId };
}

function collectDiagnostics(cdp, sessionId, sink) {
	return cdp.on((msg) => {
		if (msg.sessionId !== sessionId)
			return;

		if (msg.method === 'Runtime.consoleAPICalled') {
			const text = msg.params.args
				.map((a) => a.value ?? a.description ?? a.unserializableValue ?? '')
				.join(' ');
			sink.push({ level: msg.params.type, source: 'console', text });
		} else if (msg.method === 'Runtime.exceptionThrown') {
			const d = msg.params.exceptionDetails;
			sink.push({ level: 'error', source: 'javascript',
				    text: `uncaught: ${d.exception?.description ?? d.text}` });
		} else if (msg.method === 'Log.entryAdded') {
			const e = msg.params.entry;
			sink.push({ level: e.level, source: e.source, text: e.text });
		}
	});
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/* Resolve true when a session event arrives, false on timeout. */
function waitForEvent(cdp, sessionId, method, timeoutMs) {
	return new Promise((resolve) => {
		const done = (v) => { clearTimeout(timer); off(); resolve(v); };
		const timer = setTimeout(() => done(false), timeoutMs);
		const off = cdp.on((msg) => {
			if (msg.sessionId === sessionId && msg.method === method)
				done(true);
		});
	});
}

async function evaluate(cdp, sessionId, expression, awaitPromise = false) {
	const r = await cdp.send('Runtime.evaluate',
		{ expression, awaitPromise, returnByValue: true }, sessionId);
	if (r.exceptionDetails)
		throw new Error(r.exceptionDetails.exception?.description ?? r.exceptionDetails.text);
	return r.result.value;
}

/*
 * Wait until the injected clock reports we are a fixed number of frames past
 * first render (kruddSetReady), or until the wall-clock timeout. Deterministic
 * across backends: the clock advances per rendered frame, not per millisecond,
 * so "captureFrames past ready" is the same animation phase whether a backend
 * boots fast or slow. Returns { reached, frame, readyFrame }.
 */
async function awaitCaptureFrame(cdp, sessionId, captureFrames, timeoutMs) {
	const deadline = Date.now() + timeoutMs;
	let last = { frame: 0, readyFrame: -1 };
	while (Date.now() < deadline) {
		const c = await evaluate(cdp, sessionId,
			'window.__renderDiffClock ? window.__renderDiffClock() : null');
		if (c) {
			last = c;
			if (c.readyFrame >= 0 && c.frame >= c.readyFrame + captureFrames)
				return { reached: true, ...c };
		}
		await sleep(50);
	}
	return { reached: false, ...last };
}

/* Capture one URL, clipped to the canvas. Returns base64 PNG + diagnostics.
 * `captureFrames` is how many rendered frames past first render to settle for;
 * `timeoutMs` is the wall-clock ceiling on reaching that frame. */
async function capture(cdp, url, canvasSel, { captureFrames, timeoutMs }) {
	const { targetId, sessionId } = await openPage(cdp);
	const diags = [];
	const stopCollecting = collectDiagnostics(cdp, sessionId, diags);

	try {
		/*
		 * Page.navigate resolves when navigation *starts*, so settling from
		 * there measures from the wrong instant — a cold CDN fetch then eats
		 * the settle window and the canvas captures black with an empty log.
		 * Wait for the load event first, so settle_ms means "after load".
		 */
		const loaded = waitForEvent(cdp, sessionId, 'Page.loadEventFired', 45000);
		await cdp.send('Page.navigate', { url }, sessionId);
		if (!await loaded)
			diags.push({ level: 'error', source: 'harness',
				     text: 'page load event never fired (timeout)' });

		/*
		 * Settle on a frame count, not the wall clock, then freeze. Waiting a
		 * fixed number of frames past first render lands both backends on the
		 * same animation phase (see DETERMINISTIC_CLOCK); holding the clock
		 * keeps that phase pinned through overlay-hiding, the screenshot, and
		 * any blank-capture retry.
		 */
		const clock = await awaitCaptureFrame(cdp, sessionId, captureFrames, timeoutMs);
		await evaluate(cdp, sessionId, 'window.__renderDiffHold && window.__renderDiffHold()');
		if (!clock.reached)
			diags.push({ level: 'warning', source: 'harness',
				     text: `capture frame not reached in ${timeoutMs}ms `
					  + `(ready=${clock.readyFrame}, frame=${clock.frame}); `
					  + 'captured on the fallback timeout' });

		/*
		 * Hide the shell's overlays before capturing. They are HTML sitting
		 * over the canvas, not engine output, so clipping to the canvas does
		 * not exclude them — a screenshot catches whatever is composited
		 * there.
		 *
		 * Both are asymmetric between backends, which is what made them
		 * poisonous rather than merely noisy: the shell hides the launcher on
		 * the WebGPU path only, and the WebGPU status log appears on that path
		 * only. Together they were 42 of the 90 points the parity scene was
		 * reporting — the diff was measuring chrome, not rendering.
		 */
		await evaluate(cdp, sessionId, `
			(() => {
				const l = document.getElementById('launcher');
				if (l) l.classList.add('hidden');
				const w = document.getElementById('webgpu-log');
				if (w) w.classList.remove('visible');
				return true;
			})()`);
		await sleep(750);

		/* Clip to the canvas. The page chrome differs by backend on purpose
		 * (the header badge reads WEBGL vs WEBGPU), so a full-page diff would
		 * report a failure on every single run. */
		const box = await evaluate(cdp, sessionId, `
			(() => {
				const el = document.querySelector(${JSON.stringify(canvasSel)});
				if (!el) return null;
				const r = el.getBoundingClientRect();
				return { x: r.x, y: r.y, width: r.width, height: r.height,
					 dpr: window.devicePixelRatio || 1 };
			})()`);

		if (!box)
			throw new Error(`canvas ${canvasSel} not found — did the page load?`);
		if (box.width < 1 || box.height < 1)
			throw new Error(`canvas ${canvasSel} has zero size (${box.width}x${box.height})`);

		const clip = { x: box.x, y: box.y, width: box.width, height: box.height, scale: 1 };

		/*
		 * Retry a uniformly-blank capture. The tab-throttling flags above fix
		 * the common case, but the compositor can still hand back a frame the
		 * canvas has not presented into yet, and that shows up as a single
		 * flat colour. Retrying costs a few seconds; a false FAIL costs trust
		 * in the harness. A canvas that is *genuinely* blank still reports
		 * blank after the retries — this hides no real regression, it only
		 * refuses to call a race a result.
		 */
		let png = null;
		for (let attempt = 1; attempt <= 3; attempt++) {
			await cdp.send('Page.bringToFront', {}, sessionId);
			const shot = await cdp.send('Page.captureScreenshot', { format: 'png', clip }, sessionId);
			png = shot.data;

			if (!await isUniform(cdp, png))
				break;
			if (attempt < 3) {
				diags.push({ level: 'warning', source: 'harness',
					     text: `capture ${attempt} was a flat colour — retrying` });
				await sleep(2000);
			} else {
				diags.push({ level: 'warning', source: 'harness',
					     text: 'canvas captured as a flat colour after 3 attempts' });
			}
		}

		return { png, diags };
	} finally {
		stopCollecting();
		await cdp.send('Target.closeTarget', { targetId }).catch(() => {});
	}
}

// ------------------------------------------------------------- diffing

/*
 * Compare two PNGs inside the browser: Image decodes them, a 2D canvas gives
 * us pixels, and we emit a diff image marking changed pixels red over a dimmed
 * copy of the reference. Keeping this in-page is what lets the driver stay
 * dependency-free — no PNG library in Node.
 */
const DIFF_FN = `
async (aB64, bB64, thresh) => {
	const load = (b64) => new Promise((res, rej) => {
		const img = new Image();
		img.onload = () => res(img);
		img.onerror = () => rej(new Error('failed to decode a screenshot'));
		img.src = 'data:image/png;base64,' + b64;
	});
	const [A, B] = await Promise.all([load(aB64), load(bB64)]);
	if (A.width !== B.width || A.height !== B.height) {
		return { sizeMismatch: true,
			 a: A.width + 'x' + A.height, b: B.width + 'x' + B.height };
	}

	const w = A.width, h = A.height;
	const px = (img) => {
		const c = document.createElement('canvas');
		c.width = w; c.height = h;
		const g = c.getContext('2d', { willReadFrequently: true });
		g.drawImage(img, 0, 0);
		return g.getImageData(0, 0, w, h);
	};
	const da = px(A), db = px(B);

	const outC = document.createElement('canvas');
	outC.width = w; outC.height = h;
	const outG = outC.getContext('2d');
	const out = outG.createImageData(w, h);

	let differing = 0;
	for (let i = 0; i < da.data.length; i += 4) {
		const dr = Math.abs(da.data[i]     - db.data[i]);
		const dg = Math.abs(da.data[i + 1] - db.data[i + 1]);
		const dbl = Math.abs(da.data[i + 2] - db.data[i + 2]);
		const dal = Math.abs(da.data[i + 3] - db.data[i + 3]);
		if (Math.max(dr, dg, dbl, dal) > thresh) {
			differing++;
			out.data[i] = 255; out.data[i + 1] = 0; out.data[i + 2] = 0; out.data[i + 3] = 255;
		} else {
			const grey = (da.data[i] + da.data[i + 1] + da.data[i + 2]) / 6 + 40;
			out.data[i] = out.data[i + 1] = out.data[i + 2] = grey;
			out.data[i + 3] = 255;
		}
	}
	outG.putImageData(out, 0, 0);
	return {
		sizeMismatch: false, w, h, differing,
		total: w * h,
		fraction: differing / (w * h),
		diffPng: outC.toDataURL('image/png').split(',')[1],
	};
}
`;

/* True when every pixel is the same colour — the signature of a capture that
 * raced the compositor. Decoded in the browser, like the diff itself. */
async function isUniform(cdp, pngB64) {
	const { targetId, sessionId } = await openPage(cdp);
	try {
		const fn = `
		async (b64) => {
			const img = await new Promise((res, rej) => {
				const i = new Image();
				i.onload = () => res(i);
				i.onerror = () => rej(new Error('decode failed'));
				i.src = 'data:image/png;base64,' + b64;
			});
			const c = document.createElement('canvas');
			c.width = img.width; c.height = img.height;
			const g = c.getContext('2d', { willReadFrequently: true });
			g.drawImage(img, 0, 0);
			const d = g.getImageData(0, 0, img.width, img.height).data;
			for (let i = 4; i < d.length; i += 4) {
				if (d[i] !== d[0] || d[i+1] !== d[1] || d[i+2] !== d[2])
					return false;
			}
			return true;
		}`;
		return await evaluate(cdp, sessionId, `(${fn})(${JSON.stringify(pngB64)})`, true);
	} finally {
		await cdp.send('Target.closeTarget', { targetId }).catch(() => {});
	}
}

async function comparePngs(cdp, aB64, bB64) {
	const { targetId, sessionId } = await openPage(cdp);
	try {
		const expr = `(${DIFF_FN})(${JSON.stringify(aB64)}, ${JSON.stringify(bB64)}, ${CHANNEL_THRESHOLD})`;
		return await evaluate(cdp, sessionId, expr, true);
	} finally {
		await cdp.send('Target.closeTarget', { targetId }).catch(() => {});
	}
}

// ---------------------------------------------------------------- main

const rawArgs = process.argv.slice(2);
const args = parseArgs(rawArgs);

if (args.help) {
	console.log(USAGE.trim());
	process.exit(0);
}

ensureDisplay(rawArgs);

const manifest = JSON.parse(readFileSync(join(HERE, 'scenes.json'), 'utf8'));
const canvasSel = manifest.canvas ?? '#canvas';
let scenes = manifest.scenes;

if (args.scene) {
	scenes = scenes.filter((s) => s.id === args.scene);
	if (!scenes.length)
		die(`no scene "${args.scene}" in scenes.json`);
}

mkdirSync(OUT, { recursive: true });
mkdirSync(SHOTS, { recursive: true });

const base = args.base.endsWith('/') ? args.base : args.base + '/';
const { cdp, kill } = await launchChrome();
const results = [];

try {
	for (const scene of scenes) {
		/* How many rendered frames past first render to settle for, and the
		 * wall-clock ceiling on reaching that frame. settle_ms is a timeout
		 * now, not a fixed wait — capture returns as soon as the frame lands. */
		const captureFrames = scene.capture_frames ?? 180;
		const timeoutMs = scene.settle_ms ?? 15000;
		const url = base + (scene.query ?? '');
		process.stdout.write(`\n${scene.id}  [${scene.mode}]  ${url}\n`);

		const shot = await capture(cdp, url, canvasSel, { captureFrames, timeoutMs });
		writeFileSync(join(OUT, `${scene.id}.webgpu.png`), Buffer.from(shot.png, 'base64'));

		/* GPU validation errors are reported whether or not the pixels match —
		 * a scene can render plausibly while spraying validation errors, which
		 * is exactly the state worth catching early in the port.
		 *
		 * Network entries are printed but never fail a scene: a stray favicon
		 * 404 is noise, and a genuinely missing asset (the .wasm, say) shows up
		 * as a blank canvas and fails the pixel comparison anyway. */
		const allErrors = shot.diags.filter((d) => d.level === 'error');
		const errors = allErrors.filter((d) => d.source !== 'network');
		writeFileSync(join(OUT, `${scene.id}.log`),
			shot.diags.map((d) => `[${d.level}/${d.source}] ${d.text}`).join('\n') + '\n');

		for (const e of allErrors)
			console.log(`  ${e.source === 'network' ? 'warn ' : 'ERROR'}  ${e.text}`);

		let status = 'pass';
		let detail = '';

		if (scene.mode === 'capture') {
			detail = 'captured (no assertion)';
		} else {
			let reference = null;
			let refLabel = '';

			if (scene.mode === 'self') {
				const refPath = join(SHOTS, `${scene.id}.png`);
				if (!existsSync(refPath)) {
					status = 'fail';
					detail = `no reference shot — review out/${scene.id}.webgpu.png then re-run with --accept`;
				} else {
					reference = readFileSync(refPath).toString('base64');
					refLabel = 'reference';
				}
			} else if (scene.mode === 'diff') {
				const glUrl = base + (scene.webgl_query ?? '');
				const glShot = await capture(cdp, glUrl, canvasSel, { captureFrames, timeoutMs });
				writeFileSync(join(OUT, `${scene.id}.webgl.png`), Buffer.from(glShot.png, 'base64'));
				reference = glShot.png;
				refLabel = 'webgl';
			} else {
				die(`scene ${scene.id}: unknown mode "${scene.mode}"`);
			}

			if (reference) {
				const cmp = await comparePngs(cdp, reference, shot.png);
				if (cmp.sizeMismatch) {
					status = 'fail';
					detail = `size mismatch: ${refLabel} ${cmp.a} vs webgpu ${cmp.b}`;
				} else {
					writeFileSync(join(OUT, `${scene.id}.diff.png`),
						Buffer.from(cmp.diffPng, 'base64'));
					const pct = (cmp.fraction * 100).toFixed(3);
					const tol = scene.tolerance ?? 0;
					detail = `${cmp.differing}/${cmp.total} px differ (${pct}%), tolerance ${(tol * 100).toFixed(3)}%`;
					if (cmp.fraction > tol)
						status = 'fail';
				}
			}
		}

		/* Validation errors fail the scene on their own. */
		if (errors.length) {
			const note = `${errors.length} GPU/console error(s)`;
			detail = detail ? `${detail}; ${note}` : note;
			status = 'fail';
		}

		console.log(`  ${status === 'pass' ? 'PASS' : 'FAIL'}   ${detail}`);
		results.push({ ...scene, status, detail });

		if (args.accept && scene.mode !== 'capture') {
			writeFileSync(join(SHOTS, `${scene.id}.png`), Buffer.from(shot.png, 'base64'));
			console.log(`  accepted → shots/${scene.id}.png`);
		}
	}
} finally {
	kill();
}

const failed = results.filter((r) => r.status !== 'pass');
console.log(`\n${results.length - failed.length}/${results.length} passed`);
if (failed.length) {
	for (const f of failed)
		console.log(`  FAIL  ${f.id}: ${f.detail}`);
	console.log(`\nartifacts in tools/render-diff/out/`);
}
process.exit(failed.length ? 1 : 0);
