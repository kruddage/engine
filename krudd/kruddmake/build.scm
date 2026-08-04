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
  (call-with-input-file (rz-spec-path krudd-root dir) read))

(define manifest
  (map (lambda (dir) (cons dir (load-spec dir))) owned-directories))

;;! Read through rz-engine-root rather than spelled out, so the root the
;;! generator EMITS (ninja's `$srcroot`, and every `(root …)` path hanging off
;;! it) is the same root it READ the specs from — an SDK build that resolved its
;;! build.scm files under the prefix and then compiled against `krudd/engine`
;;! would be two builds wearing one manifest (#1035).
(define src-root (rz-engine-root krudd-root))

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

;;! KRUDD_TARGET picks which of ninja.scm's three phony targets this build
;;! drives: "wasm" for the WASM module, "archives" for the native libraries
;;! alone (no test binaries — the SDK's linux-x86_64 archives, #1035), and
;;! anything else (including unset) for "native", the whole native suite,
;;! unchanged from before this option existed.
(define ninja-target
  (let ((target (getenv "KRUDD_TARGET")))
    (cond ((and target (string=? target "wasm")) "wasm")
          ((and target (string=? target "archives")) "archives")
          (else "native"))))

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
                       ninja-target)))
