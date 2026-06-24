cmake_minimum_required(VERSION 3.20)
project(krudd C)

set(CMAKE_C_STANDARD 11)

add_executable(index modules/core/engine.c)

target_include_directories(index PRIVATE modules/core)

if(EMSCRIPTEN)
	set_target_properties(index PROPERTIES SUFFIX ".html")
	target_link_options(index PRIVATE
		-sENVIRONMENT=web
		-sALLOW_MEMORY_GROWTH=1
	)
endif()
