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
; output). Every directory under cmake/ is owned now; grow this manifest and
; drop a CMakeLists.scm beside a future directory's sources to strangle it too.
;
; The root CMakeLists.txt is owned too now (krudd/cmake/CMakeLists.scm, beside
; the emitter rather than under cmake/ since it describes the tree instead of
; living in it): its layout, options and flags are data, and the imperative
; project()/git/FetchContent bootstrap rides through a (verbatim ...) block
; until it too becomes forms.

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
    "modules/core"
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
    "plugins/imgui_plugin"
    "plugins/kruddboard"))

;; Read a directory's spec (a bare datum — no evaluation) from its spec file.
(define (load-spec dir)
  (call-with-input-file
    (string-append krudd-root "/cmake/" dir "/CMakeLists.scm")
    read))

;; Synthesize the root cmake/CMakeLists.txt from its spec. Unlike the leaf
;; directories the root is not in owned-directories — it is the tree it points
;; at — so it is rendered on its own. The spec itself lives at
;; krudd/cmake/CMakeLists.scm, beside cmake.scm, since it describes the tree
;; rather than living in it.
(define (synthesize-root)
  (let ((source "krudd/cmake/CMakeLists.scm")
	(out    (string-append krudd-root "/cmake/CMakeLists.txt")))
    (display (string-append "krudd: synthesize " out "\n"))
    (write-file out
		(cmake-synthesize
		  source
		  (call-with-input-file
		    (string-append krudd-root "/" source)
		    read)))))

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

(synthesize-root)
(synthesize-owned)
(sh *configure*)
(sh *build*)
