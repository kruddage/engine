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
; CMakeLists.txt for the directories krudd has taken ownership of, from the
; specs below. CMake is now a backend we emit for, not the source of truth.
; Each directory we move behind a spec is one more root strangling the vine;
; the rest still ships its hand-written CMakeLists.txt until its turn comes.

(define krudd-root (or (getenv "KRUDD_ROOT") "."))

(load (string-append krudd-root "/krudd/cmake.scm"))

(define (sh cmd)
  (let ((status (run cmd)))
    (if (not (= status 0))
	(error 'krudd-build-failed cmd))))

(define (write-file path text)
  (call-with-output-file path
    (lambda (port) (write-string text port))))

;; ---------------------------------------------------------------------------
;; Specs. Each entry is (cmake-relative-dir . directory-spec). The path is
;; relative to cmake/, matching the add_subdirectory() layout in the root
;; CMakeLists.txt. Grow this list to strangle another directory.
;; ---------------------------------------------------------------------------

(define owned-directories
  (list
    (cons "modules/log"
	  '((library "log"
	      (sources "log.c" (root "modules/core/ring_buf.c"))
	      (public "include" (root "plugins/include"))
	      (private (root "modules/core/include")))
	    (native-only
	      (executable "log_test"
		(sources "log_test.c")
		(link "log"))
	      (test "log" "log_test"))))

    ;; memory.c calls the libc malloc family directly. On WASM the main module
    ;; is linked -sMALLOC=mimalloc (see modules/core), so that libc — and this
    ;; seam — is mimalloc; natively it is the platform libc. No separately
    ;; linked allocator: one heap for the whole program.
    (cons "modules/memory"
	  '((library "memory"
	      (sources "memory.c")
	      (public "include"))
	    (native-only
	      (executable "memory_test"
		(sources "memory_test.c")
		(link "memory"))
	      (test "memory" "memory_test"))))

    ;; math has no library — the math sources compile straight into the native
    ;; test (and, on WASM, into scene_renderer's side module).
    (cons "plugins/math"
	  '((native-only
	      (executable "math_test"
		(sources "math_test.c" "math.c" "camera.c")
		(private (root "plugins/include"))
		(link "m"))
	      (test "math" "math_test"))))

    ;; The renderer GPU API is a header-only INTERFACE seam.
    (cons "plugins/renderer"
	  '((interface-library "renderer_interface"
	      (interface "."))))

    ;; Content-addressed copy-on-write store (#214) — the substrate-agnostic
    ;; core. cas.c is the addressing/manifest logic; cas_mem.c is the
    ;; native/test backing (the IndexedDB backing is browser-only).
    (cons "plugins/cas"
	  '((library "cas"
	      (sources "cas.c")
	      (public "." (root "plugins/include")))
	    (native-only
	      (executable "cas_test"
		(sources "cas_test.c" "cas_mem.c")
		(private "." (root "plugins/include")
			 (root "modules/memory/include"))
		(link "cas" "memory"))
	      (test "cas" "cas_test"))))

    ;; Branch model + bootstrap + live-save + switch (#215) over the CAS (#214).
    (cons "plugins/branch"
	  '((library "branch"
	      (sources "branch.c")
	      (public "." (root "plugins/include") (root "plugins/cas"))
	      (link "cas"))
	    (native-only
	      (executable "branch_test"
		(sources "branch_test.c" (root "plugins/cas/cas_mem.c"))
		(private "." (root "plugins/include") (root "plugins/cas")
			 (root "modules/memory/include"))
		(link "branch" "cas" "memory"))
	      (test "branch" "branch_test"))))

    ;; Per-branch auto-snapshots + restore (#216) over the branch model (#215)
    ;; and the CAS (#214).
    (cons "plugins/snapshot"
	  '((library "snapshot"
	      (sources "snapshot.c")
	      (public "." (root "plugins/include") (root "plugins/cas")
		      (root "plugins/branch"))
	      (link "branch" "cas"))
	    (native-only
	      (executable "snapshot_test"
		(sources "snapshot_test.c" "snapshot.c"
			 (root "plugins/branch/branch.c")
			 (root "plugins/cas/cas.c")
			 (root "plugins/cas/cas_mem.c"))
		(private "." (root "plugins/include") (root "plugins/cas")
			 (root "plugins/branch")
			 (root "modules/memory/include"))
		(link "memory"))
	      (test "snapshot" "snapshot_test"))))

    ;; Minimal example plugin — a side module and nothing else.
    (cons "plugins/hello_plugin"
	  '((side-module "hello_plugin"
	      (target "hello_plugin")
	      (includes (root "modules/core/include"))
	      (sources (current "hello_plugin.c"))
	      (depends (current "hello_plugin.c")))))

    (cons "plugins/scene_plugin"
	  '((library "scene_plugin"
	      (sources "scene_plugin.c")
	      (public "." (root "plugins/include") (root "modules/core/include"))
	      (link "memory" "subsystem_manager"))
	    (native-only
	      (executable "scene_test"
		(sources "scene_test.c")
		(link "scene_plugin" "asset_plugin" "log" "memory"
		      "subsystem_manager"))
	      (test "scene" "scene_test"))
	    (side-module "scene_plugin"
	      (includes (current) (root "modules/core/include")
			(root "plugins/include"))
	      (sources (current "scene_plugin.c"))
	      (depends (current "scene_plugin.c")))))

    (cons "plugins/vscript"
	  '((library "vscript_plugin"
	      (sources "vscript.c")
	      (public "." (root "plugins/include") (root "modules/core/include"))
	      (link "memory" "subsystem_manager"))
	    (native-only
	      (executable "vscript_test"
		(sources "vscript_test.c")
		(link "vscript_plugin" "asset_plugin" "log" "memory"
		      "subsystem_manager"))
	      (test "vscript" "vscript_test"))
	    (side-module "vscript"
	      (includes (current) (root "modules/core/include")
			(root "plugins/include"))
	      (sources (current "vscript.c"))
	      (depends (current "vscript.c")))))

    (cons "plugins/shader_graph"
	  '((library "shader_graph_plugin"
	      (sources "shader_nodes.c" "glsl_backend.c" "shader_graph.c")
	      (public "." (root "plugins/include") (root "modules/core/include"))
	      (link "memory" "subsystem_manager"))
	    (native-only
	      (executable "shader_graph_test"
		(sources "shader_graph_test.c")
		(link "shader_graph_plugin" "vscript_plugin" "asset_plugin"
		      "log" "memory" "subsystem_manager"))
	      (test "shader_graph" "shader_graph_test"))
	    (side-module "shader_graph"
	      (includes (current) (root "modules/core/include")
			(root "plugins/include"))
	      (sources (current "shader_nodes.c") (current "glsl_backend.c")
		       (current "shader_graph.c"))
	      (depends (current "shader_nodes.c") (current "glsl_backend.c")
		       (current "shader_graph.c")))))

    (cons "plugins/edit_plugin"
	  '((library "edit_plugin"
	      (sources "edit.c" "edit_plugin.c")
	      (public "." (root "plugins/include") (root "modules/core/include"))
	      (link "subsystem_manager"))
	    (native-only
	      ;; Link the pure history ops directly so the test needs no glue.
	      (executable "edit_test"
		(sources "edit_test.c" "edit.c")
		(private "." (root "plugins/include"))
		(link "memory"))
	      (test "edit" "edit_test"))
	    (side-module "edit_plugin"
	      (includes (current) (root "modules/core/include")
			(root "plugins/include"))
	      (sources (current "edit.c") (current "edit_plugin.c"))
	      (depends (current "edit.c") (current "edit_plugin.c")))))

    (cons "plugins/entity_plugin"
	  '((library "entity_plugin"
	      (sources "entity.c" "entity_plugin.c" "scene_edit.c")
	      (public "." (root "plugins/include") (root "modules/core/include"))
	      (link "memory" "subsystem_manager"))
	    (native-only
	      ;; Pure world ops linked directly — no plugin glue in the test.
	      (executable "entity_test"
		(sources "entity_test.c" "entity.c")
		(private "." (root "plugins/include")
			 (root "modules/core/include")
			 (root "modules/memory/include"))
		(link "memory"))
	      (test "entity" "entity_test")
	      ;; Snapshot-based undo: world ops + edit history linked directly
	      ;; so the test drives the same command path as the plugin.
	      (executable "scene_edit_test"
		(sources "scene_edit_test.c" "scene_edit.c" "entity.c"
			 (root "plugins/edit_plugin/edit.c"))
		(private "." (root "plugins/include")
			 (root "plugins/edit_plugin")
			 (root "modules/core/include")
			 (root "modules/memory/include"))
		(link "memory"))
	      (test "scene_edit" "scene_edit_test"))
	    (side-module "entity_plugin"
	      (includes (current) (root "modules/core/include")
			(root "plugins/include"))
	      (sources (current "entity.c") (current "entity_plugin.c")
		       (current "scene_edit.c"))
	      (depends (current "entity.c") (current "entity_plugin.c")
		       (current "scene_edit.c")))))

    (cons "plugins/renderer_null"
	  '((library "renderer_null"
	      (sources "renderer_null.c")
	      (private (root "plugins/renderer"))
	      (link "log" "subsystem" "subsystem_manager"))
	    (native-only
	      (executable "renderer_null_test"
		(sources "renderer_null_test.c")
		(private "." (root "plugins/renderer"))
		(link "renderer_null" "log" "subsystem_manager"))
	      (test "renderer_null" "renderer_null_test"))
	    (side-module "renderer_null"
	      (includes (current) (root "plugins/renderer")
			(root "modules/core/include") (root "plugins/include"))
	      (sources (current "renderer_null.c"))
	      (depends (current "renderer_null.c")))))

    (cons "plugins/renderer_webgl"
	  '((library "renderer_webgl"
	      (sources "renderer_webgl.c")
	      (private (root "plugins/renderer"))
	      (link "log" "memory" "subsystem" "subsystem_manager"))
	    (side-module "renderer_webgl"
	      (includes (current) (root "plugins/renderer")
			(root "modules/core/include") (root "plugins/include"))
	      (sources (current "renderer_webgl.c"))
	      (depends (current "renderer_webgl.c")))))

    (cons "plugins/frame_graph"
	  '((library "frame_graph"
	      (sources "fg.c")
	      (public ".")
	      (private (root "plugins/renderer"))
	      (link "log" "memory" "subsystem" "subsystem_manager"))
	    (native-only
	      (executable "fg_test"
		(sources "fg_test.c")
		(private "." (root "plugins/renderer")
			 (root "plugins/renderer_null"))
		(link "frame_graph" "renderer_null" "log" "memory"
		      "subsystem_manager"))
	      (test "fg" "fg_test"))
	    (side-module "frame_graph"
	      (includes (current) (root "plugins/renderer")
			(root "modules/core/include") (root "plugins/include"))
	      (sources (current "fg.c"))
	      (depends (current "fg.c")))))

    (cons "plugins/scene_renderer"
	  '((native-only
	      (executable "scene_renderer_test"
		(sources "scene_renderer_test.c" "scene_renderer.c"
			 (root "plugins/math/math.c")
			 (root "plugins/math/camera.c")
			 (root "plugins/asset/primitives.c"))
		(private "." (root "plugins/renderer")
			 (root "plugins/renderer_null")
			 (root "plugins/frame_graph") (root "plugins/asset")
			 (root "plugins/include"))
		(link "frame_graph" "renderer_null" "log" "memory"
		      "subsystem_manager" "m"))
	      (test "scene_renderer" "scene_renderer_test"))
	    (side-module "scene_renderer"
	      (includes (current) (root "plugins/renderer")
			(root "plugins/frame_graph")
			(root "modules/core/include") (root "plugins/include"))
	      (sources (current "scene_renderer.c")
		       (root "plugins/math/math.c")
		       (root "plugins/math/camera.c"))
	      (depends (current "scene_renderer.c")
		       (root "plugins/math/math.c")
		       (root "plugins/math/camera.c")
		       (root "plugins/include/mesh.h")))))

    (cons "plugins/asset"
	  '((library "asset_plugin"
	      (sources "asset_plugin.c" "primitives.c" "asset_edit.c")
	      (public "." (root "plugins/include"))
	      (link "log" "memory" "subsystem" "subsystem_manager" "m"))
	    (native-only
	      (executable "asset_test" (sources "asset_test.c")
		(link "asset_plugin" "log" "memory"))
	      (test "asset" "asset_test")
	      (executable "asset_codec_test" (sources "asset_codec_test.c")
		(link "asset_plugin" "log" "memory"))
	      (test "asset_codec" "asset_codec_test")
	      (executable "asset_api_test" (sources "asset_api_test.c")
		(link "asset_plugin" "log" "memory"))
	      (test "asset_api" "asset_api_test")
	      (executable "asset_mut_test" (sources "asset_mut_test.c")
		(link "asset_plugin" "log" "memory"))
	      (test "asset_mut" "asset_mut_test")
	      ;; Undo/redo recording for authored assets: drive the same record
	      ;; path the plugin uses (asset_edit lives in asset_plugin) against
	      ;; the real catalog, with the edit history linked directly.
	      (executable "asset_edit_test"
		(sources "asset_edit_test.c" (root "plugins/edit_plugin/edit.c"))
		(private (root "plugins/edit_plugin"))
		(link "asset_plugin" "log" "memory"))
	      (test "asset_edit" "asset_edit_test")
	      (executable "asset_shader_test" (sources "asset_shader_test.c")
		(link "asset_plugin" "log" "memory"))
	      (test "asset_shader" "asset_shader_test")
	      (executable "asset_mesh_test" (sources "asset_mesh_test.c")
		(link "asset_plugin" "log" "memory"))
	      (test "asset_mesh" "asset_mesh_test"))
	    (side-module "asset_plugin"
	      (includes (current) (root "modules/core/include")
			(root "plugins/include"))
	      (sources (current "asset_plugin.c") (current "primitives.c")
		       (current "asset_edit.c"))
	      (depends (current "asset_plugin.c") (current "primitives.c")
		       (current "primitives.h") (current "asset_edit.c")
		       (current "asset_edit.h")
		       (root "plugins/include/mesh.h")))))))

;; Synthesize every owned directory's CMakeLists.txt, then let CMake build.
(define (synthesize-owned)
  (for-each
    (lambda (entry)
      (let ((path (string-append krudd-root "/cmake/" (car entry)
				 "/CMakeLists.txt")))
	(display (string-append "krudd: synthesize " path "\n"))
	(write-file path (cmake-synthesize (cdr entry)))))
    owned-directories))

(synthesize-owned)
(sh *configure*)
(sh *build*)
