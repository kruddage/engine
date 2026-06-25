cmake_minimum_required(VERSION 3.20)
project(krudd C)

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

add_subdirectory(modules/log)
add_subdirectory(modules/memory)
add_subdirectory(modules/core)
add_subdirectory(plugins/asset)
add_subdirectory(plugins/hello_plugin)
add_subdirectory(plugins/renderer)
add_subdirectory(plugins/renderer_null)
add_subdirectory(plugins/renderer_webgpu)
add_subdirectory(plugins/frame_graph)

if(NOT EMSCRIPTEN)
	set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
	enable_testing()
	add_subdirectory(tests)
endif()
