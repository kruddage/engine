; SPDX-License-Identifier: GPL-2.0-or-later
;; scm-lint:off
;
; runtime.scm — the Scheme image the engine boots into, live inside the WASM.
;
; This is the seam where the substrate inverts: the engine's C main() still
; owns the literal emscripten main loop, but the body of the frame is handed to
; the (tick) defined here. krudd-log is a primitive backed by the engine's log
; subsystem; the plugin ABI is meant to grow more primitives beside it so game
; and engine logic can move into this file. Baked into the module at build time
; (see krudd-embed-file), so today it ships read-only — a live REPL into this
; image is the next step.
;; scm-lint:on

(krudd-log 1 "runtime: Scheme image live — krudd.scm owns the tick")

(define *frame* 0)

(define (tick)
  (set! *frame* (+ *frame* 1))
  (when (= 0 (modulo *frame* 60))
    (krudd-log 0 (string-append "runtime: tick " (number->string *frame*)))))
