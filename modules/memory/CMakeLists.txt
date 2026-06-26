add_library(memory memory.c)
target_include_directories(memory PUBLIC include)
target_link_libraries(memory PRIVATE mimalloc-static)

if(NOT EMSCRIPTEN)
	add_executable(memory_test memory_test.c)
	target_link_libraries(memory_test PRIVATE memory)
	add_test(NAME memory COMMAND memory_test)
endif()
