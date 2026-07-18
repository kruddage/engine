# render-diff

A screenshot oracle for the WebGPU port. It drives Chrome, captures the engine
canvas under each backend, compares the pixels, and — just as importantly —
collects the WebGPU validation errors the browser would otherwise swallow.

```sh
node tools/render-diff/diff.mjs --base https://kruddage.github.io/engine/
```

Zero dependencies. The DevTools Protocol transport is Node's built-in
`WebSocket`, and PNG decoding happens inside the browser, which is already a PNG
codec — so there is no `node_modules` in the tree and nothing to keep up to date.

## It must not run headless

**Headless Chrome composites WebGPU canvases as blank.** The canvas captures as
pure white even while the page reports the device is live and drawing.

This is browser behaviour, not an engine bug — the official
[webgpu-samples helloTriangle](https://webgpu.github.io/webgpu-samples/?sample=helloTriangle)
reproduces it exactly. And it is a trap rather than an inconvenience, because
**WebGL captures fine in headless**: the obvious sanity check ("is my screenshot
tool working? yes, WebGL renders") actively confirms a broken instrument.

So the harness runs headful on a real display, and re-execs itself under
`xvfb-run` when `DISPLAY` is unset. There is deliberately no `--headless` flag;
a mode that silently reports "nothing rendered" is worse than no harness at all.

Verified working on Chrome 149 with the nvidia Vulkan ICD.

## Why the error capture matters

A WebGPU validation failure usually surfaces as a blank canvas plus a message
that never reaches the page's console — indistinguishable from "nothing was
drawn", and invisible to anyone driving the browser from outside. That is how a
port ends up debugged blind.

So before any page script runs, the harness injects a shim that wraps
`GPUAdapter.prototype.requestDevice` and forwards `uncapturederror` and
`device.lost` to `console.error`, where the driver collects them alongside
console output and uncaught exceptions. Full diagnostics land in
`out/<id>.log`; error lines echo to stdout.

Errors are reported **even when the pixel comparison passes** — a scene can
render plausibly while spraying validation errors, which is exactly the state
worth catching early. A scene with GPU errors fails regardless of its diff.

Network entries (a stray favicon 404) are printed but never fail a scene; a
genuinely missing asset shows up as a blank canvas and fails on pixels anyway.

## Modes

Scenes live in `scenes.json`, each with a mode. They graduate as the vtable
lands:

- **`capture`** — screenshot, assert nothing. For a scene that does not render
  under both backends yet. Eyeball the output.
- **`self`** — compare WebGPU against its own accepted reference in `shots/`.
  Catches regressions in a path that has no counterpart to compare to.
- **`diff`** — compare WebGPU against WebGL, the reference implementation.
  The real oracle: *does my port match yet?*

Today there is one scene — the WGSL triangle probe, mode `self`. Under
`?renderer=webgpu` the engine registers the WebGPU backend alone and skips the
whole GL cluster (`engine.c`), so WebGPU draws a triangle where WebGL draws the
entire engine. **There is no comparable scene until `scene_renderer` runs on the
WebGPU path.** `diff` mode is built and waiting for it, not dead code.

## Usage

```
--base <url>    site root (default http://127.0.0.1:8000)
--scene <id>    run one scene instead of all
--accept        promote this run's output to the reference shots
```

Against a local build:

```sh
KRUDD_TARGET=wasm ./krudd.sh build
python3 -m http.server -d build 8000
node tools/render-diff/diff.mjs
```

Against a PR preview — **no local emsdk needed**, since CI already builds every
PR:

```sh
node tools/render-diff/diff.mjs --base https://kruddage.github.io/engine/pr-preview/pr-123/
```

`--accept` is the deliberate "this new rendering is correct" gesture. Look at
`out/<id>.webgpu.png` first; it overwrites the committed reference.

Exit status is non-zero if any scene fails.

## Output

`out/` (gitignored) gets `<id>.webgpu.png`, `<id>.webgl.png` in `diff` mode,
`<id>.diff.png` marking changed pixels red over a dimmed reference, and
`<id>.log`. `shots/` holds the committed references.

## Known intermittent

In one run out of roughly twenty-five during development, a scene failed with
two console/GPU errors while its pixels matched perfectly. It did not reproduce
across 18 consecutive runs afterwards and the log was not preserved, so **the
cause is unknown** — a transient device-lost under GPU contention is a guess,
not a diagnosis.

Left as-is deliberately: the harness already writes full diagnostics to
`out/<id>.log` and echoes error lines, so the next occurrence is
self-documenting. If it recurs, that log is the thing to read.

## Notes

- Screenshots are clipped to the canvas. The page chrome differs by backend on
  purpose — the header badge reads `WEBGL` vs `WEBGPU` — so a full-page diff
  would fail on every run.
- Capture size follows the browser window (1400x900), so a reference taken at a
  different size fails loudly with a size mismatch rather than silently
  rescaling.
- The triangle probe is static and compares bit-exact between runs; the default
  0.2% tolerance is headroom for animated scenes later. If live-tick animation
  makes tolerances too noisy to be useful, the fix is an engine-side hook that
  runs a fixed number of ticks and holds, which the harness would call before
  capturing.
