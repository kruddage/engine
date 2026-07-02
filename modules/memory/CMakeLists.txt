add_library(memory memory.c)
target_include_directories(memory PUBLIC include)
target_link_libraries(memory PRIVATE mimalloc-static)

# On WASM the main module is linked -sMALLOC=none; this shim provides the libc
# malloc family on top of mimalloc so the whole program (engine, libc, FETCH,
# the dynamic linker, and every side module) shares one heap. Native builds use
# the platform libc and never compile it. -fno-builtin: don't assume standard
# semantics for the functions we are replacing.
if(EMSCRIPTEN)
	target_sources(memory PRIVATE mi_libc_shim.c)
	set_source_files_properties(mi_libc_shim.c PROPERTIES
		COMPILE_OPTIONS "-fno-builtin")
endif()

if(NOT EMSCRIPTEN)
	add_executable(memory_test memory_test.c)
	target_link_libraries(memory_test PRIVATE memory)
	add_test(NAME memory COMMAND memory_test)
endif()
