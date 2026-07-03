# Per-branch auto-snapshots + restore (#216) over the branch model (#215) and
# the content-addressed store (#214). Pure logic; the runtime debounced capture
# and the re-ingest on restore layer on top.
add_library(snapshot snapshot.c)
target_include_directories(snapshot PUBLIC .
	${CMAKE_SOURCE_DIR}/plugins/include
	${CMAKE_SOURCE_DIR}/plugins/cas
	${CMAKE_SOURCE_DIR}/plugins/branch)
target_link_libraries(snapshot PRIVATE branch cas)

if(NOT EMSCRIPTEN)
	add_executable(snapshot_test snapshot_test.c snapshot.c
		${CMAKE_SOURCE_DIR}/plugins/branch/branch.c
		${CMAKE_SOURCE_DIR}/plugins/cas/cas.c
		${CMAKE_SOURCE_DIR}/plugins/cas/cas_mem.c)
	target_include_directories(snapshot_test PRIVATE .
		${CMAKE_SOURCE_DIR}/plugins/include
		${CMAKE_SOURCE_DIR}/plugins/cas
		${CMAKE_SOURCE_DIR}/plugins/branch)
	add_test(NAME snapshot COMMAND snapshot_test)
endif()
