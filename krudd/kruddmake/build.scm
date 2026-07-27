; SPDX-License-Identifier: GPL-2.0-or-later

(define krudd-root (or (getenv "KRUDD_ROOT") "."))

(load (string-append krudd-root "/krudd/kruddmake/ninja.scm"))

(define (sh cmd)
  (let ((status (run cmd)))
    (if (not (= status 0))
        (error 'krudd-build-failed cmd))))

(define (write-file path text)
  (call-with-output-file path
    (lambda (port) (write-string text port))))

(define owned-directories
  (call-with-input-file
      (string-append krudd-root "/krudd/kruddmake/manifest.scm")
    read))

(define (load-spec dir)
  (call-with-input-file
      (string-append krudd-root "/krudd/engine/" dir "/build.scm")
    read))

(define manifest
  (map (lambda (dir) (cons dir (load-spec dir))) owned-directories))

(define src-root (string-append krudd-root "/krudd/engine"))

;;! KRUDD_BUILD_DIR points the generated build.ninja and its objects at a
;;! variant-specific directory. run-tests.sh has always honoured it — that is
;;! how the sanitizer and coverage jobs keep instrumented objects out of the
;;! plain build's tree — but `krudd build` did not, so the two entry points into
;;! the same generator disagreed about where output lands. They no longer do.
;;! It rides on regen-cmd below so the `regen` edge targets the same directory
;;! the build it re-enters was configured with.
(define build-dir
  (let ((dir (getenv "KRUDD_BUILD_DIR")))
    (if (and dir (> (string-length dir) 0))
        dir
        (string-append krudd-root "/build"))))

(define wasm-build?
  (let ((target (getenv "KRUDD_TARGET")))
    (and target (string=? target "wasm"))))

(sh (string-append "mkdir -p \"" build-dir "\""))

;;! The command ninja re-runs (via the `regen` generator edge) when a codegen
;;! input changes: regenerate build.ninja + codegen, but stop short of driving
;;! ninja again — KRUDD_GENERATE_ONLY breaks that recursion.
(define regen-cmd
  (string-append "env KRUDD_ROOT=" krudd-root
                 " KRUDD_BUILD_DIR=" build-dir
                 " KRUDD_GENERATE_ONLY=1 "
                 krudd-root "/krudd/krudd build"))

(display (string-append "krudd: generate " build-dir "/build.ninja\n"))
(write-file (string-append build-dir "/build.ninja")
            (ninja-synthesize manifest src-root build-dir regen-cmd))

(if (not (getenv "KRUDD_GENERATE_ONLY"))
    (sh (string-append "ninja -C \"" build-dir "\" -f build.ninja "
                       (if wasm-build? "wasm" "native"))))
