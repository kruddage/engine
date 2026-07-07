; SPDX-License-Identifier: GPL-2.0-or-later
;
; krudd build description — the build authority, in Scheme.
;
; krudd (see ../krudd.c) provides:
;   (run cmd)     -> run a shell command, return its integer exit status
;   *configure*   -> the configure command string (cmake / emcmake cmake ...)
;   *build*       -> the build command string
;
; The strangler fig, phase 2: before we hand off to CMake we *synthesize* the
; CMakeLists.txt for the directories krudd has taken ownership of. CMake is now
; a backend we emit for, not the source of truth. Each owned directory carries
; its own CMakeLists.scm — pure data (a list of target forms), living beside
; the sources it describes — which cmake.scm renders to CMake text. krudd/
; stays the tool (this orchestrator, the s7 runtime, the CMake-backend
; emitter); cmake/ stays the project (sources, specs, and the generated
; output). Grow the owned-directories manifest and drop a CMakeLists.scm into
; the directory to strangle another one; the rest still ships its
; hand-written CMakeLists.txt until its turn comes.

(define krudd-root (or (getenv "KRUDD_ROOT") "."))

(load (string-append krudd-root "/krudd/cmake/cmake.scm"))

(define (sh cmd)
  (let ((status (run cmd)))
    (if (not (= status 0))
	(error 'krudd-build-failed cmd))))

(define (write-file path text)
  (call-with-output-file path
    (lambda (port) (write-string text port))))

;; ---------------------------------------------------------------------------
;; The manifest: every directory krudd owns, as a path relative to cmake/ (which
;; matches the add_subdirectory() layout in the root CMakeLists.txt). The spec
;; for each lives at cmake/<dir>/CMakeLists.scm; the output at cmake/<dir>/
;; CMakeLists.txt. Keep this list in sync with .gitignore.
;; ---------------------------------------------------------------------------

(define owned-directories
  (list
    "modules/log"
    "modules/memory"
    "plugins/math"
    "plugins/renderer"
    "plugins/cas"
    "plugins/branch"
    "plugins/snapshot"
    "plugins/hello_plugin"
    "plugins/scene_plugin"
    "plugins/vscript"
    "plugins/shader_graph"
    "plugins/edit_plugin"
    "plugins/entity_plugin"
    "plugins/renderer_null"
    "plugins/renderer_webgl"
    "plugins/frame_graph"
    "plugins/scene_renderer"
    "plugins/asset"
    "plugins/backend"
    "plugins/imgui_plugin"))

;; Read a directory's spec (a bare datum — no evaluation) from its spec file.
(define (load-spec dir)
  (call-with-input-file
    (string-append krudd-root "/cmake/" dir "/CMakeLists.scm")
    read))

;; Synthesize every owned directory's CMakeLists.txt, then let CMake build.
(define (synthesize-owned)
  (for-each
    (lambda (dir)
      (let ((source (string-append "cmake/" dir "/CMakeLists.scm"))
	    (out    (string-append krudd-root "/cmake/" dir
				   "/CMakeLists.txt")))
	(display (string-append "krudd: synthesize " out "\n"))
	(write-file out (cmake-synthesize source (load-spec dir)))))
    owned-directories))

(synthesize-owned)
(sh *configure*)
(sh *build*)
