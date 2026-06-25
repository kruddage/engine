cmake_minimum_required(VERSION 3.14)
project(mimalloc C)

# Header-only stub — all functions are static inline in mimalloc.h.
# Provide an empty mimalloc-static target so the consumer link command works.
add_library(mimalloc-static INTERFACE)
target_include_directories(mimalloc-static INTERFACE
	$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
