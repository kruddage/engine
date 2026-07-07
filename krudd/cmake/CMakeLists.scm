; SPDX-License-Identifier: GPL-2.0-or-later
;
; Root build spec — the top-level CMakeLists.txt as krudd data.
;
; The leaf directories were pure target lists; this one is project scaffolding.
; The whole thing is now forms krudd renders (see krudd/cmake/cmake.scm): the
; layout, the C standard, the coverage/sanitizer options, the global warning
; flags and the native-only test wiring, and — since #341 — the bootstrap that
; used to ride through (verbatim ...) too. The version read, the git build-number
; and commit-hash introspection, and the imgui fetch are done at synthesis time
; by krudd/introspect.scm and baked into the output, rather than emitted as
; CMake's file(READ)/execute_process/FetchContent for the backend to run. That
; keeps this spec free of imperative CMake, so the Ninja backend can render it
; from the same forms.
;
; Rendered to krudd/cmake/CMakeLists.txt by ./krudd.sh build. Do not edit the
; .txt.

((cmake-minimum "3.20")

 ; The top-level CMakeLists.txt lives at krudd/cmake/ while VERSION,
 ; CHANGELOG.md and the rest of the repo metadata stay at the repo root, two
 ; levels up. Leaf bootstrap still references ${KRUDD_REPO_ROOT}, so keep it.
 (repo-root "KRUDD_REPO_ROOT")

 ; VERSION is read and stripped at synthesis time; project() carries the literal.
 (project "krudd" "VERSION" "C" "CXX")

 ; ENGINE_BUILD_NUMBER / GIT_COMMIT_HASH from git, defaults baked if git is
 ; absent. Consumed by modules/core's version.h.in and shell.html.in.
 (git-build-info)

 (set "CMAKE_C_STANDARD" "11")

 (option "COVERAGE" "Enable gcov code coverage" "OFF"
	(compile-options "--coverage")
	(link-options "--coverage"))

 (option "SANITIZE" "Enable AddressSanitizer and UndefinedBehaviorSanitizer" "OFF"
	(compile-options "-fsanitize=address,undefined" "-fno-omit-frame-pointer")
	(link-options "-fsanitize=address,undefined"))

 ; mimalloc is not linked separately: on WASM it is supplied by the toolchain via
 ; -sMALLOC=mimalloc (modules/core), and modules/memory allocates through libc,
 ; so there is a single heap. Native builds use the platform libc.
 ;
 ; imgui is only needed for the WASM side-module builds, so it is fetched (cloned
 ; at the pinned tag) only when synthesizing an EMSCRIPTEN build; ${imgui_SOURCE_DIR}
 ; is what imgui_plugin/kruddboard reference.
 (fetch-content "imgui" "https://github.com/ocornut/imgui.git" "v1.90.9"
	"EMSCRIPTEN")

 (compile-options "-Wall" "-Werror" "-Wpedantic")

 (native-only
	(set "CMAKE_EXPORT_COMPILE_COMMANDS" "ON")
	(enable-testing))

 (subdirs
	"modules/log"
	"modules/memory"
	"modules/core"
	"plugins/math"
	"plugins/asset"
	"plugins/scene_plugin"
	"plugins/vscript"
	"plugins/shader_graph"
	"plugins/entity_plugin"
	"plugins/edit_plugin"
	"plugins/hello_plugin"
	"plugins/renderer"
	"plugins/renderer_null"
	"plugins/renderer_webgl"
	"plugins/frame_graph"
	"plugins/scene_renderer"
	"plugins/imgui_plugin"
	"plugins/backend"
	"plugins/cas"
	"plugins/branch"
	"plugins/snapshot"
	"plugins/kruddboard"))
