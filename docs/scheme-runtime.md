# Scheme Runtime — s7 in the engine

Design note for the first step of the "Scheme owns the program" direction.

## Motivation

krudd, the build tool, is already an s7 Scheme program: `krudd.sh` compiles
`krudd.c` together with `third_party/s7.c` and lets Scheme (`build.scm`) render
the build and drive `ninja`. Until now that interpreter lived **only** at build
time — `s7.c` was compiled with the host compiler into the `krudd` binary and
never reached the runtime WASM.

The direction is to invert the substrate: make Scheme the language the engine
and games are written in, and drop to C (and later Rust) only for the paths that
must be fast. This note covers the first, deliberately small step — getting the
same interpreter to ship inside `index.wasm` and hand it the body of the frame.

## What ships

- **`modules/core/script.{c,h}`** — the runtime binding. `script_init()` starts
  s7 and registers the engine primitives; `script_eval()` loads a Scheme image
  (reading and evaluating every top-level form, not just the first); and
  `script_tick()` hands each frame to the image's `(tick)` if one is defined.
- **`modules/core/runtime.scm`** — the Scheme image the engine boots into. It is
  embedded into the module at build time (the same `krudd-embed-file` codegen
  that bakes `CHANGELOG.md` into the "What's New" tab), so today it ships
  read-only.
- **One primitive, `(krudd-log level text)`**, backed by the log subsystem — the
  proof that a value produced inside the Scheme image crosses back into the
  engine's C services.

## The seam

C `main()` still owns the literal `emscripten_set_main_loop` callback; what
changes is that `engine_tick` now runs `script_tick()`, so the *body* of the
frame is Scheme, not C. This is the honest halfway point of the inversion: the
loop is still lent by C, but the frame's logic is the image's to define. The hot
inner work (entity updates, the render graph, math) stays in C/Rust primitives
that Scheme calls — an interpreter has no business in a per-entity inner loop.

## Build integration

`s7.c` is a third-party amalgamation that cannot pass the project's
`-Werror -Wpedantic` rules, so the emitter (`ninja.scm`) compiles it with its
own relaxed rules — `-w -DWITH_C_LOADER=0 -DWITH_MAIN=0`, matching how
`krudd.sh` builds it for the build tool — and folds the object into the
`script` archive, native (`cc_s7`) and WASM (`emcc_s7`) alike. A bare
`(link "script")` therefore drags the whole interpreter in, for the native unit
test and the WASM main module both.

## Verification

The `script` library and its native test (`script_test.c`) build and run in the
native suite: a Scheme image defines a `(tick)` that logs, the engine calls
`script_tick()`, and the test asserts the message reached the log ring buffer.
The WASM link (s7 through `emcc`, linked into `index.wasm` alongside mimalloc)
is exercised by CI, which builds the Emscripten target.

## Next steps

- A live REPL into the running image (kruddboard is the natural host) — redefine
  a procedure and watch the frame change without a reload.
- More of the plugin ABI exposed as primitives (entity, asset, renderer).
- Load the image as an asset through the backend seam rather than baking it in,
  so game content changes without an `emcc` rebuild.
- A native engine loop, and a native filesystem backend provider, behind the
  existing `#ifdef __EMSCRIPTEN__` and `backend_api` seams.
