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
FetchContent_MakeAvailable(mimalloc)

add_library(log modules/log/log.c)
target_include_directories(log PUBLIC modules/log)

add_library(memory modules/memory/memory.c)
target_include_directories(memory PUBLIC modules/memory)
target_link_libraries(memory PRIVATE mimalloc-static)

add_library(subsystem modules/core/subsystem.c)
target_include_directories(subsystem PUBLIC modules/core)

add_executable(index modules/core/engine.c)
target_include_directories(index PRIVATE modules/core)
target_link_libraries(index PRIVATE subsystem log memory)

if(EMSCRIPTEN)
	set_target_properties(index PROPERTIES SUFFIX ".html")
	target_link_options(index PRIVATE
		-sENVIRONMENT=web
		-sALLOW_MEMORY_GROWTH=1
		-sMALLOC=emmalloc
	)
endif()

if(NOT EMSCRIPTEN)
	enable_testing()
	add_executable(memory_test tests/memory_test.c)
	target_link_libraries(memory_test PRIVATE memory)
	add_test(NAME memory COMMAND memory_test)
	add_executable(log_test tests/log_test.c)
	target_link_libraries(log_test PRIVATE log)
	add_test(NAME log COMMAND log_test)
	add_executable(subsystem_test tests/subsystem_test.c)
	target_link_libraries(subsystem_test PRIVATE subsystem)
	add_test(NAME subsystem COMMAND subsystem_test)
endif()
