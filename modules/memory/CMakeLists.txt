add_library(memory memory.c)
target_include_directories(memory PUBLIC .)
target_link_libraries(memory PRIVATE mimalloc-static)
