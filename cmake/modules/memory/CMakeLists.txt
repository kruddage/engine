# memory.c calls the libc malloc family directly. On WASM the main module is
# linked -sMALLOC=mimalloc (see modules/core/CMakeLists.txt), so that libc — and
# therefore this seam — is mimalloc; natively it is the platform libc. No
# separately-linked allocator: one heap for the whole program.
add_library(memory memory.c)
target_include_directories(memory PUBLIC include)

if(NOT EMSCRIPTEN)
	add_executable(memory_test memory_test.c)
	target_link_libraries(memory_test PRIVATE memory)
	add_test(NAME memory COMMAND memory_test)
endif()
