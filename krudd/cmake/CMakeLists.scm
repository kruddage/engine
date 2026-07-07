; SPDX-License-Identifier: GPL-2.0-or-later
;
; Root build spec — the top-level CMakeLists.txt as krudd data.
;
; The leaf directories were pure target lists; this one is project scaffolding.
; The parts that are data — the C standard, the coverage/sanitizer options, the
; global warning flags, the native-only test wiring, and above all the
; subdirectory layout — are modelled as forms krudd renders (see
; krudd/cmake/cmake.scm). The parts that are imperative CMake bootstrap —
; reading VERSION, project(), the git build-number/commit-hash introspection,
; and the imgui FetchContent — are not yet strangled and ride through
; (verbatim ...) unchanged. When their turn comes, they become forms too and the
; verbatim blocks shrink to nothing.
;
; Rendered to krudd/cmake/CMakeLists.txt by ./krudd.sh build. Do not edit the
; .txt.

((verbatim "cmake_minimum_required(VERSION 3.20)

# The top-level CMakeLists.txt lives at krudd/cmake/ while VERSION,
# CHANGELOG.md and the rest of the repo metadata stay at the repo root, two
# levels up.
get_filename_component(KRUDD_REPO_ROOT \"${CMAKE_CURRENT_SOURCE_DIR}/../..\" ABSOLUTE)

file(READ \"${KRUDD_REPO_ROOT}/VERSION\" _krudd_version_raw)
string(STRIP \"${_krudd_version_raw}\" _krudd_version)

project(krudd VERSION ${_krudd_version} LANGUAGES C CXX)

find_package(Git QUIET)
set(ENGINE_BUILD_NUMBER 0)
set(GIT_COMMIT_HASH \"unknown\")
if(GIT_FOUND)
	execute_process(
		COMMAND ${GIT_EXECUTABLE} log -1 --format=%H
			-S \"${PROJECT_VERSION}\" -- VERSION
		WORKING_DIRECTORY ${KRUDD_REPO_ROOT}
		OUTPUT_VARIABLE _version_anchor
		OUTPUT_STRIP_TRAILING_WHITESPACE
		ERROR_QUIET
	)
	if(_version_anchor)
		execute_process(
			COMMAND ${GIT_EXECUTABLE} rev-list --count HEAD ^${_version_anchor}
			WORKING_DIRECTORY ${KRUDD_REPO_ROOT}
			OUTPUT_VARIABLE ENGINE_BUILD_NUMBER
			OUTPUT_STRIP_TRAILING_WHITESPACE
			ERROR_QUIET
		)
	endif()
	if(NOT ENGINE_BUILD_NUMBER)
		set(ENGINE_BUILD_NUMBER 0)
	endif()
	execute_process(
		COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
		WORKING_DIRECTORY ${KRUDD_REPO_ROOT}
		OUTPUT_VARIABLE GIT_COMMIT_HASH
		OUTPUT_STRIP_TRAILING_WHITESPACE
		ERROR_QUIET
	)
	if(NOT GIT_COMMIT_HASH)
		set(GIT_COMMIT_HASH \"unknown\")
	endif()
endif()")

 (set "CMAKE_C_STANDARD" "11")

 (option "COVERAGE" "Enable gcov code coverage" "OFF"
	(compile-options "--coverage")
	(link-options "--coverage"))

 (option "SANITIZE" "Enable AddressSanitizer and UndefinedBehaviorSanitizer" "OFF"
	(compile-options "-fsanitize=address,undefined" "-fno-omit-frame-pointer")
	(link-options "-fsanitize=address,undefined"))

 (verbatim "include(FetchContent)

# mimalloc is not linked separately: on WASM it is supplied by the toolchain via
# -sMALLOC=mimalloc (modules/core/CMakeLists.txt), and modules/memory allocates
# through libc, so there is a single heap. Native builds use the platform libc.

# imgui is only needed for the WASM side-module builds.
if(EMSCRIPTEN)
	FetchContent_Declare(imgui
		GIT_REPOSITORY https://github.com/ocornut/imgui.git
		GIT_TAG        v1.90.9
		GIT_SHALLOW    TRUE
	)
	FetchContent_MakeAvailable(imgui)
endif()")

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
