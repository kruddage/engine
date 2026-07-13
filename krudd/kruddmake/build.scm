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
(define build-dir (string-append krudd-root "/build"))

(define wasm-build?
  (let ((target (getenv "KRUDD_TARGET")))
    (and target (string=? target "wasm"))))

(if wasm-build?
    (krudd-fetch "imgui" "https://github.com/ocornut/imgui.git" "v1.90.9"))

(sh (string-append "mkdir -p " build-dir))

;;! The command ninja re-runs (via the `regen` generator edge) when a codegen
;;! input changes: regenerate build.ninja + codegen, but stop short of driving
;;! ninja again — KRUDD_GENERATE_ONLY breaks that recursion.
(define regen-cmd
  (string-append "env KRUDD_ROOT=" krudd-root " KRUDD_GENERATE_ONLY=1 "
		 krudd-root "/krudd/krudd build"))

(display (string-append "krudd: generate " build-dir "/build.ninja\n"))
(write-file (string-append build-dir "/build.ninja")
	    (ninja-synthesize manifest src-root build-dir regen-cmd))

(if (not (getenv "KRUDD_GENERATE_ONLY"))
    (sh (string-append "ninja -C " build-dir " -f build.ninja "
		       (if wasm-build? "wasm" "native"))))
