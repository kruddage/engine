cmake_minimum_required(VERSION 3.20)
project(krudd C)

set(CMAKE_C_STANDARD 11)

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

if(NOT EMSCRIPTEN)
	add_subdirectory(tests)
endif()
