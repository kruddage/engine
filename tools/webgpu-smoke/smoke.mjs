#!/usr/bin/env node
// SPDX-License-Identifier: GPL-2.0-or-later
//
// webgpu-smoke — does the engine actually render under WebGPU?
//
// A deliberately small, binary gate: load the page under a backend at a given
// devicePixelRatio and fail if either
//
//   (a) any WebGPU validation error fires, or
//   (b) the canvas comes back blank.
//
// This exists because a total WebGPU blackout (#622) shipped through eight
// commits with nothing watching. render-diff can see that class of bug but is
// hand-run, and its parity percentage is not stable enough to gate on — it
// swings several points run to run, so a threshold would be flaky from day
// one. "Zero validation errors and the canvas is not blank" is deterministic.
//
//   node tools/webgpu-smoke/smoke.mjs --base <url> [--dpr 1,3] [--renderer webgpu]
//
// Must run on a real display, for the same reason render-diff must: headless
// Chrome composites a WebGPU canvas blank while reporting the device is live,
// so headless would confirm a broken instrument. With DISPLAY unset this
// re-execs under xvfb-run.

import { spawn, spawnSync } from 'node:child_process';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

// ---------------------------------------------------------------- args

function arg(name, fallback) {
	const i = process.argv.indexOf(`--${name}`);
	return i > 0 && process.argv[i + 1] ? process.argv[i + 1] : fallback;
}

const BASE      = arg('base', 'http://127.0.0.1:8000/index.html');
const RENDERER  = arg('renderer', 'webgpu');
const DPRS      = arg('dpr', '1,3').split(',').map((s) => Number(s.trim()));
const SETTLE_MS = Number(arg('settle', '9000'));

/*
 * A phone-shaped viewport on purpose. The dpr 3 case is the one a desktop
 * cannot see on its own: at devicePixelRatio 1, CSS pixels and device pixels
 * are the same number, so a whole class of canvas-sizing bug (#610) is
 * structurally invisible. Emulation is not a substitute for a real Adreno or
 * Mali — it catches sizing and validation bugs, not driver divergence.
 */
const VIEW_W = 412;
const VIEW_H = 883;

/*
 * One known-benign validation error, fired once per page load at the
 * probe->cluster handover. It predates the bloom regression this gate was
 * written for, does not affect the picture, and its cause is still unknown —
 * the obvious hypothesis (backbuffer_size re-assigning the canvas size) was
 * built, measured and disproved. Tracked in #649.
 *
 * Matched narrowly and on purpose: this is the ONLY tolerated error, it is
 * pinned to the 300x150 default-canvas texture, and it is allowed at most once
 * per configuration. Anything else — including a second copy of this one — is a
 * failure. Delete this the day #649 lands; an allowlist that outlives its bug
 * quietly stops being a gate.
 */
const KNOWN_BENIGN = /Destroyed texture \[Texture \(unlabeled 300x150 px/;

/* Mean luminance below this reads as "nothing was drawn". The engine's scenes
 * are dark, so this is deliberately near the floor: the failure being caught
 * is a canvas that never presented at all, not a dim one. */
const BLANK_MEAN = 1.5;

// ------------------------------------------------- the xvfb requirement

if (!process.env.DISPLAY) {
	const probe = spawnSync('sh', ['-c', 'command -v xvfb-run'], { encoding: 'utf8' });
	if (probe.status !== 0) {
		console.error('webgpu-smoke: DISPLAY is unset and xvfb-run is not installed.\n' +
			      '  Headless Chrome composites a WebGPU canvas blank while reporting\n' +
			      '  the device is live, so this harness refuses to run without a\n' +
			      '  display. Install xvfb, or run under a real X session.');
		process.exit(2);
	}
	console.log('webgpu-smoke: no DISPLAY — re-execing under xvfb-run');
	const r = spawnSync('xvfb-run',
			    ['-a', '-s', `-screen 0 1400x900x24`, process.argv0, ...process.argv.slice(1)],
			    { stdio: 'inherit' });
	process.exit(r.status ?? 1);
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/*
 * A WebGPU validation failure usually surfaces as a blank canvas plus a message
 * that never reaches the page console, so it is indistinguishable from "nothing
 * was drawn". Wrap requestDevice before any page script runs and forward
 * uncaptured errors to console.error, where the driver below collects them.
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

// ------------------------------------------------------- chrome + CDP

async function launchChrome() {
	const profile = mkdtempSync(join(tmpdir(), 'webgpu-smoke-'));
	const bin = process.env.CHROME_BIN || '/usr/bin/google-chrome';

	const child = spawn(bin, [
		'--remote-debugging-port=0',
		`--user-data-dir=${profile}`,
		'--enable-unsafe-webgpu',
		'--enable-features=Vulkan',
		'--use-angle=vulkan',
		'--no-first-run',
		'--no-default-browser-check',
		/* A backgrounded or occluded tab gets its rendering throttled, which
		 * stops the frame loop and captures the canvas black — a false
		 * failure that looks exactly like the real one this gate hunts. */
		'--disable-backgrounding-occluded-windows',
		'--disable-renderer-backgrounding',
		'--disable-background-timer-throttling',
		'--window-size=1400,900',
		'about:blank',
	], { stdio: ['ignore', 'ignore', 'pipe'] });

	const wsUrl = await new Promise((resolve, reject) => {
		let buf = '';
		const timer = setTimeout(() => reject(new Error('chrome did not report a DevTools endpoint')), 20000);
		child.stderr.on('data', (chunk) => {
			buf += chunk.toString();
			const m = buf.match(/DevTools listening on (ws:\/\/\S+)/);
			if (m) { clearTimeout(timer); resolve(m[1]); }
		});
		child.on('exit', (code) => {
			clearTimeout(timer);
			reject(new Error(`chrome exited early (${code}); is ${bin} installed?`));
		});
	});

	return {
		wsUrl,
		kill: () => {
			child.kill();
			try { rmSync(profile, { recursive: true, force: true }); } catch { /* best effort */ }
		},
	};
}

class CDP {
	constructor(ws) { this.ws = ws; this.id = 0; this.pending = new Map(); this.listeners = []; }

	static async connect(url) {
		const ws = new WebSocket(url);
		await new Promise((res, rej) => { ws.onopen = res; ws.onerror = rej; });
		const c = new CDP(ws);
		ws.onmessage = (e) => {
			const msg = JSON.parse(e.data);
			if (msg.id && c.pending.has(msg.id)) {
				const { resolve, reject } = c.pending.get(msg.id);
				c.pending.delete(msg.id);
				msg.error ? reject(new Error(msg.error.message)) : resolve(msg.result);
			} else {
				c.listeners.forEach((fn) => fn(msg));
			}
		};
		return c;
	}

	send(method, params = {}, sessionId) {
		const id = ++this.id;
		return new Promise((resolve, reject) => {
			this.pending.set(id, { resolve, reject });
			this.ws.send(JSON.stringify({ id, method, params, ...(sessionId ? { sessionId } : {}) }));
		});
	}

	on(fn) { this.listeners.push(fn); }
}

async function evaluate(cdp, sessionId, expression) {
	const r = await cdp.send('Runtime.evaluate',
				 { expression, returnByValue: true, awaitPromise: true },
				 sessionId);
	if (r.exceptionDetails)
		throw new Error(r.exceptionDetails.text);
	return r.result.value;
}

// ---------------------------------------------------------- one probe

async function probe(cdp, { renderer, dpr }) {
	const errors = [];

	const { targetId } = await cdp.send('Target.createTarget', { url: 'about:blank' });
	const { sessionId } = await cdp.send('Target.attachToTarget', { targetId, flatten: true });

	await cdp.send('Page.enable', {}, sessionId);
	await cdp.send('Runtime.enable', {}, sessionId);
	await cdp.send('Page.addScriptToEvaluateOnNewDocument', { source: GPU_ERROR_SHIM }, sessionId);

	cdp.on((msg) => {
		if (msg.sessionId !== sessionId) return;
		if (msg.method === 'Runtime.consoleAPICalled') {
			const text = msg.params.args
				.map((a) => a.value ?? a.description ?? '')
				.join(' ');
			if (text.includes('[gpu-error]')) errors.push(text);
		} else if (msg.method === 'Runtime.exceptionThrown') {
			/* .text is just "Uncaught (in promise)" for a rejected
			 * promise; the useful part is the exception's own
			 * description. Keep both or the report says nothing. */
			const d = msg.params.exceptionDetails;
			const detail = d.exception?.description ?? d.exception?.value ?? '';
			errors.push(`[exception] ${d.text}${detail ? ': ' + detail : ''}`);
		}
	});

	await cdp.send('Emulation.setDeviceMetricsOverride',
		       { width: VIEW_W, height: VIEW_H, deviceScaleFactor: dpr, mobile: dpr !== 1 },
		       sessionId);

	await cdp.send('Page.navigate', { url: `${BASE}?renderer=${renderer}` }, sessionId);
	await sleep(SETTLE_MS);

	/* The launcher and any status panel are HTML composited OVER the canvas —
	 * they paint whether or not a single triangle was drawn, so a screenshot
	 * that includes them can read "not blank" on a completely dead renderer.
	 * Hide them, then clip to the canvas. */
	await evaluate(cdp, sessionId, `
		(() => {
			const l = document.getElementById('launcher');
			if (l) l.classList.add('hidden');
			const w = document.getElementById('webgpu-log');
			if (w) w.classList.remove('visible');
			return true;
		})()`);
	await sleep(500);

	const box = await evaluate(cdp, sessionId, `
		(() => {
			const el = document.querySelector('canvas');
			if (!el) return null;
			const r = el.getBoundingClientRect();
			return { x: r.x, y: r.y, width: r.width, height: r.height,
				 bufW: el.width, bufH: el.height };
		})()`);

	if (!box)
		throw new Error('no canvas on the page — did it load?');
	if (box.width < 1 || box.height < 1)
		throw new Error(`canvas has zero size (${box.width}x${box.height})`);

	const shot = await cdp.send('Page.captureScreenshot', {
		format: 'png',
		clip: { x: box.x, y: box.y, width: box.width, height: box.height, scale: 1 },
	}, sessionId);

	/*
	 * Measure the SCREENSHOT, never the live canvas. drawImage() off a WebGL
	 * canvas without preserveDrawingBuffer — and off a WebGPU canvas after
	 * present — returns blank for a perfectly healthy frame. Reading it that
	 * way reports every backend as black and makes the harness the bug.
	 * Decoding happens in the browser, which is already a PNG codec.
	 */
	const stats = await evaluate(cdp, sessionId, `
		(async () => {
			const img = new Image();
			img.src = 'data:image/png;base64,${shot.data}';
			await img.decode();
			const c = document.createElement('canvas');
			c.width = 160; c.height = 160;
			const g = c.getContext('2d');
			g.drawImage(img, 0, 0, 160, 160);
			const d = g.getImageData(0, 0, 160, 160).data;
			let sum = 0, max = 0;
			for (let i = 0; i < d.length; i += 4) {
				const l = (d[i] + d[i+1] + d[i+2]) / 3;
				sum += l; if (l > max) max = l;
			}
			return { mean: sum / (d.length / 4), max };
		})()`);

	await cdp.send('Target.closeTarget', { targetId });

	return { renderer, dpr, box, stats, errors };
}

// ---------------------------------------------------------------- main

const { wsUrl, kill } = await launchChrome();
const cdp = await CDP.connect(wsUrl);
let failed = 0;

for (const dpr of DPRS) {
	let r;
	try {
		r = await probe(cdp, { renderer: RENDERER, dpr });
	} catch (e) {
		console.log(`FAIL ${RENDERER} @ dpr ${dpr}: ${e.message}`);
		failed++;
		continue;
	}

	/* Tolerate the single known-benign error, and only the first of it. */
	let benignSeen = 0;
	const real = r.errors.filter((e) => {
		if (KNOWN_BENIGN.test(e) && benignSeen++ === 0) return false;
		return true;
	});

	const blank = r.stats.mean < BLANK_MEAN;
	const bad   = real.length > 0 || blank;

	console.log(`${bad ? 'FAIL' : 'ok  '} ${RENDERER} @ dpr ${dpr} — ` +
		    `canvas ${r.box.width}x${r.box.height} css / ${r.box.bufW}x${r.box.bufH} backing, ` +
		    `mean ${r.stats.mean.toFixed(2)}, ${real.length} gpu error(s)` +
		    `${benignSeen ? ` (+${benignSeen} known-benign, #649)` : ''}`);

	if (blank)
		console.log(`     canvas is blank (mean ${r.stats.mean.toFixed(2)} < ${BLANK_MEAN})`);

	/* Print unique messages only: one broken pipeline produces the same
	 * validation error every frame, and hundreds of identical lines bury
	 * whatever else fired. */
	[...new Set(real.map((e) => e.split('\n')[0]))]
		.slice(0, 10)
		.forEach((e) => console.log(`     ${e}`));

	if (bad) failed++;
}

kill();

if (failed) {
	console.log(`\nwebgpu-smoke: ${failed} configuration(s) failed`);
	process.exit(1);
}
console.log('\nwebgpu-smoke: all configurations rendered cleanly');
process.exit(0);
