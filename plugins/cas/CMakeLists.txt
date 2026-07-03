# Content-addressed copy-on-write store (#214) — the substrate-agnostic core.
# cas.c is the addressing/manifest logic; the in-memory backing (cas_mem.c) is
# the native/test binding, and the IndexedDB binding lands later as the browser
# implementation of struct cas_backing.
add_library(cas cas.c)
target_include_directories(cas PUBLIC .
	${CMAKE_SOURCE_DIR}/plugins/include)

if(NOT EMSCRIPTEN)
	add_executable(cas_test cas_test.c cas_mem.c)
	target_include_directories(cas_test PRIVATE .
		${CMAKE_SOURCE_DIR}/plugins/include)
	target_link_libraries(cas_test PRIVATE cas)
	add_test(NAME cas COMMAND cas_test)
endif()
