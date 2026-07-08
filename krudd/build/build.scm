; SPDX-License-Identifier: GPL-2.0-or-later
;
; krudd build description — the build authority, in Scheme.
;
; krudd (see ../krudd.c) provides:
;   (run cmd)     -> run a shell command, return its integer exit status
;
; The strangler fig, complete: krudd owns the whole build. It reads the
; backend-agnostic directory specs (krudd/build/ninja/**/build.scm), renders a
; build.ninja from them (krudd/build/ninja/ninja.scm), and drives ninja(1)
; directly. Ninja stays as the executor; krudd owns the generation.
;
; Two targets, selected by KRUDD_TARGET (unset defaults to native):
;   native  cc/ar — every static library and test; the test stamps run the
;           suite, so a green build is a green test run.
;   wasm    emcc/em++ — the main module (index.html/.js/.wasm) and the plugin
;           side modules, plus the imgui fetch and the configure_file/embed
;           codegen krudd owns.
;
; Everything is generated into and built under build/.

(define krudd-root (or (getenv "KRUDD_ROOT") "."))

(load (string-append krudd-root "/krudd/build/ninja/ninja.scm"))

(define (sh cmd)
  (let ((status (run cmd)))
    (if (not (= status 0))
	(error 'krudd-build-failed cmd))))

(define (write-file path text)
  (call-with-output-file path
    (lambda (port) (write-string text port))))

;; ---------------------------------------------------------------------------
;; The manifest: every directory krudd owns, relative to krudd/build/ninja/ (a
;; bare datum in krudd/build/ninja/manifest.scm — the one list of owned
;; directories). Each carries a build.scm spec beside its sources.
;; ---------------------------------------------------------------------------

(define owned-directories
  (call-with-input-file
    (string-append krudd-root "/krudd/build/ninja/manifest.scm")
    read))

(define (load-spec dir)
  (call-with-input-file
    (string-append krudd-root "/krudd/build/ninja/" dir "/build.scm")
    read))

(define manifest
  (map (lambda (dir) (cons dir (load-spec dir))) owned-directories))

;; The tree root the spec paths resolve against, and the build directory.
(define src-root (string-append krudd-root "/krudd/build/ninja"))
(define build-dir (string-append krudd-root "/build"))

;; Target selection: KRUDD_TARGET=wasm selects the WASM output; anything else
;; (including unset) builds native.
(define wasm-build?
  (let ((target (getenv "KRUDD_TARGET")))
    (and target (string=? target "wasm"))))

;; The WASM side modules need imgui; fetch it (idempotent) before generating.
(if wasm-build?
    (krudd-fetch "imgui" "https://github.com/ocornut/imgui.git" "v1.90.9"))

(sh (string-append "mkdir -p " build-dir))

(display (string-append "krudd: generate " build-dir "/build.ninja\n"))
(write-file (string-append build-dir "/build.ninja")
	    (ninja-synthesize manifest src-root build-dir))

(sh (string-append "ninja -C " build-dir " -f build.ninja "
		   (if wasm-build? "wasm" "native")))
