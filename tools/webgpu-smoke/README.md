# webgpu-smoke

Does the engine actually render under WebGPU?

```
node tools/webgpu-smoke/smoke.mjs --base http://127.0.0.1:8000/index.html --dpr 1,3
```

Exit 0 if every configuration rendered; exit 1 if any produced a WebGPU
validation error or a blank canvas.

## Why this exists

A **total WebGPU blackout shipped and survived eight commits.** The bloom post
chain (#622) left the renderer drawing nothing at all — on every device, at
every `devicePixelRatio`, desktop included — and nothing caught it:

- the native suite cannot see the browser path at all;
- `render-diff` *can*, but is hand-run and was not run;
- WebGL was unaffected, so the bug read like a mobile-only report.

This is the standing gate that closes that hole.

## What it asserts, and what it deliberately does not

It asserts two things, both binary:

1. **Zero WebGPU validation errors** (one narrowly-matched known-benign error is
   tolerated — see `KNOWN_BENIGN` in the source, tracked in #649).
2. **The canvas is not blank.**

It does **not** assert a `render-diff` parity percentage. That number currently
swings several points between runs of an identical build, so a threshold gate
would be flaky from its first day. Parity stays a hand-run progress meter;
this stays a gate. Keep them separate.

The error check is the load-bearing one. Blankness is a weaker signal than it
looks, because the kruddgui HUD draws into the same canvas and lifts the mean
off the floor even when the 3D scene is entirely missing.

## Traps

**Never run this headless.** Headless Chrome composites a WebGPU canvas blank
while reporting the device is live — so a headless run fails a healthy build and
"confirms" a bug that is actually the instrument. The harness re-execs itself
under `xvfb-run` when `DISPLAY` is unset; do not add `--headless`.

**Never measure the live canvas with `drawImage`.** A WebGL canvas without
`preserveDrawingBuffer`, and a WebGPU canvas after present, both return blank
pixels for a perfectly healthy frame. Measuring that way reports *every* backend
as black. This harness measures the PNG from `Page.captureScreenshot`, decoded
in the browser. The first draft of this tool got this wrong and called WebGL
broken; the WebGL control is what caught it.

**Hide the launcher before capturing.** It is HTML composited over the canvas
and paints whether or not a single triangle was drawn.

**dpr 3 is not optional.** At `devicePixelRatio` 1 the CSS and device pixel
counts are identical, which makes a whole class of canvas-sizing bug (#610)
structurally invisible. A harness that only runs at dpr 1 is not measuring what
users see.

**Emulation is not a device.** dpr 3 under lavapipe or NVIDIA catches sizing and
validation bugs. It does not catch Adreno or Mali driver divergence. This gate
shrinks the search space; it does not replace a real phone.

## Serving

Point `--base` at a **staged** site (`.github/scripts/stage-site.sh`), not the
raw `build/` tree. `index.html` cache-busts the module to
`index.<short-sha>.wasm`, which only staging produces — serving `build/`
directly 404s the wasm and dies with "both async and sync fetching of the wasm
failed", which looks nothing like a missing file.
