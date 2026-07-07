; SPDX-License-Identifier: GPL-2.0-or-later
;
; krudd build description — the build authority, in Scheme.
;
; krudd (see ../krudd.c) provides:
;   (run cmd)     -> run a shell command, return its integer exit status
;   *configure*   -> the configure command string (legacy; only its emcmake
;                    marker is still read, as a fallback target hint)
;   *build*       -> the build command string (unused now)
;
; The strangler fig, complete: krudd owns the whole build. It reads the same
; backend-agnostic directory specs (krudd/cmake/**/CMakeLists.scm) it always
; has, renders a build.ninja from them (krudd/ninja/ninja.scm), and drives
; ninja(1) directly — no CMake, no emcmake. Ninja stays as the executor; krudd
; owns the generation.
;
; Two targets, selected by KRUDD_TARGET (falling back to the emcmake marker in
; *configure* for the legacy CI env):
;   native  cc/ar — every static library and test; the test stamps run the
;           suite, so a green build is a green test run.
;   wasm    emcc/em++ — the main module (index.html/.js/.wasm) and the plugin
;           side modules, plus the imgui fetch and the configure_file/changelog
;           codegen krudd owns.
;
; Everything is generated into and built under build/.

(define krudd-root (or (getenv "KRUDD_ROOT") "."))

(load (string-append krudd-root "/krudd/ninja/ninja.scm"))

(define (sh cmd)
  (let ((status (run cmd)))
    (if (not (= status 0))
	(error 'krudd-build-failed cmd))))

(define (write-file path text)
  (call-with-output-file path
    (lambda (port) (write-string text port))))

;; ---------------------------------------------------------------------------
;; The manifest: every directory krudd owns, relative to krudd/cmake/ (a bare
;; datum in krudd/cmake/manifest.scm, shared with nothing else now that CMake is
;; gone but kept as the one list of owned directories). Each carries a
;; CMakeLists.scm spec beside its sources.
;; ---------------------------------------------------------------------------

(define owned-directories
  (call-with-input-file
    (string-append krudd-root "/krudd/cmake/manifest.scm")
    read))

(define (load-spec dir)
  (call-with-input-file
    (string-append krudd-root "/krudd/cmake/" dir "/CMakeLists.scm")
    read))

(define manifest
  (map (lambda (dir) (cons dir (load-spec dir))) owned-directories))

;; The tree root the spec paths resolve against, and the build directory.
(define src-root (string-append krudd-root "/krudd/cmake"))
(define build-dir (string-append krudd-root "/build"))

;; Target selection: KRUDD_TARGET=wasm|native wins; otherwise fall back to the
;; emcmake marker the legacy CI env still carries in *configure*.
(define wasm-build?
  (let ((target (getenv "KRUDD_TARGET")))
    (if (and target (> (string-length target) 0))
	(string=? target "wasm")
	(krudd-emscripten-build?))))

;; The WASM side modules need imgui; fetch it (idempotent) before generating.
(if wasm-build?
    (krudd-fetch "imgui" "https://github.com/ocornut/imgui.git" "v1.90.9"))

(sh (string-append "mkdir -p " build-dir))

(display (string-append "krudd: generate " build-dir "/build.ninja\n"))
(write-file (string-append build-dir "/build.ninja")
	    (ninja-synthesize manifest src-root build-dir))

(sh (string-append "ninja -C " build-dir " -f build.ninja "
		   (if wasm-build? "wasm" "native")))
