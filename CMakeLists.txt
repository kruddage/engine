cmake_minimum_required(VERSION 3.20)
project(krudd C CXX)

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
FetchContent_Declare(mimalloc
	GIT_REPOSITORY https://github.com/microsoft/mimalloc.git
	GIT_TAG        v2.1.7
)
set(MI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(MI_BUILD_OBJECT OFF CACHE BOOL "" FORCE)
set(MI_BUILD_TESTS  OFF CACHE BOOL "" FORCE)
set(MI_OVERRIDE     OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(mimalloc)

FetchContent_Declare(imgui
	GIT_REPOSITORY https://github.com/ocornut/imgui.git
	GIT_TAG        v1.90.9
	GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(imgui)

if(NOT EMSCRIPTEN)
	set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
	enable_testing()
endif()

add_subdirectory(modules/log)
add_subdirectory(modules/memory)
add_subdirectory(modules/core)
add_subdirectory(plugins/asset)
add_subdirectory(plugins/hello_plugin)
add_subdirectory(plugins/renderer)
add_subdirectory(plugins/renderer_null)
add_subdirectory(plugins/renderer_webgl)
add_subdirectory(plugins/frame_graph)
add_subdirectory(plugins/imgui_plugin)
