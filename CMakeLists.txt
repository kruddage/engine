cmake_minimum_required(VERSION 3.20)

file(READ "${CMAKE_SOURCE_DIR}/VERSION" _krudd_version_raw)
string(STRIP "${_krudd_version_raw}" _krudd_version)

project(krudd VERSION ${_krudd_version} LANGUAGES C CXX)

find_package(Git QUIET)
set(ENGINE_BUILD_NUMBER 0)
set(GIT_COMMIT_HASH "unknown")
if(GIT_FOUND)
	execute_process(
		COMMAND ${GIT_EXECUTABLE} log -1 --format=%H
			-S "${PROJECT_VERSION}" -- VERSION
		WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
		OUTPUT_VARIABLE _version_anchor
		OUTPUT_STRIP_TRAILING_WHITESPACE
		ERROR_QUIET
	)
	if(_version_anchor)
		execute_process(
			COMMAND ${GIT_EXECUTABLE} rev-list --count HEAD ^${_version_anchor}
			WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
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
		WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
		OUTPUT_VARIABLE GIT_COMMIT_HASH
		OUTPUT_STRIP_TRAILING_WHITESPACE
		ERROR_QUIET
	)
	if(NOT GIT_COMMIT_HASH)
		set(GIT_COMMIT_HASH "unknown")
	endif()
endif()

set(CMAKE_C_STANDARD 11)

option(COVERAGE "Enable gcov code coverage" OFF)
if(COVERAGE)
	add_compile_options(--coverage)
	add_link_options(--coverage)
endif()

option(SANITIZE "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(SANITIZE)
	add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
	add_link_options(-fsanitize=address,undefined)
endif()

include(FetchContent)

# Use the vendored stub for native builds; fetch real mimalloc for WASM.
if(NOT EMSCRIPTEN)
	set(FETCHCONTENT_SOURCE_DIR_MIMALLOC
		${CMAKE_SOURCE_DIR}/vendor/mimalloc-stub)
endif()
FetchContent_Declare(mimalloc
	GIT_REPOSITORY https://github.com/microsoft/mimalloc.git
	GIT_TAG        v2.1.7
)
set(MI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(MI_BUILD_OBJECT OFF CACHE BOOL "" FORCE)
set(MI_BUILD_TESTS  OFF CACHE BOOL "" FORCE)
set(MI_OVERRIDE     OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(mimalloc)

# imgui is only needed for the WASM side-module builds.
if(EMSCRIPTEN)
	FetchContent_Declare(imgui
		GIT_REPOSITORY https://github.com/ocornut/imgui.git
		GIT_TAG        v1.90.9
		GIT_SHALLOW    TRUE
	)
	FetchContent_MakeAvailable(imgui)
endif()

add_compile_options(-Wall -Werror -Wpedantic)

if(NOT EMSCRIPTEN)
	set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
	enable_testing()
endif()

add_subdirectory(modules/log)
add_subdirectory(modules/memory)
add_subdirectory(modules/core)
add_subdirectory(plugins/math)
add_subdirectory(plugins/asset)
add_subdirectory(plugins/scene_plugin)
add_subdirectory(plugins/entity_plugin)
add_subdirectory(plugins/edit_plugin)
add_subdirectory(plugins/hello_plugin)
add_subdirectory(plugins/renderer)
add_subdirectory(plugins/renderer_null)
add_subdirectory(plugins/renderer_webgl)
add_subdirectory(plugins/frame_graph)
add_subdirectory(plugins/imgui_plugin)
add_subdirectory(plugins/backend)
add_subdirectory(plugins/kruddboard)
