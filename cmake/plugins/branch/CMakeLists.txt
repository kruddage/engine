# Branch model + bootstrap + live-save + switch mechanism (#215) over the
# content-addressed store (#214). Pure logic; the runtime wiring (ingest into
# the world, swap the catalog, publish as a backend capability) layers on top.
add_library(branch branch.c)
target_include_directories(branch PUBLIC .
	${CMAKE_SOURCE_DIR}/plugins/include
	${CMAKE_SOURCE_DIR}/plugins/cas)
target_link_libraries(branch PRIVATE cas)

if(NOT EMSCRIPTEN)
	add_executable(branch_test branch_test.c
		${CMAKE_SOURCE_DIR}/plugins/cas/cas_mem.c)
	target_include_directories(branch_test PRIVATE .
		${CMAKE_SOURCE_DIR}/plugins/include
		${CMAKE_SOURCE_DIR}/plugins/cas
		${CMAKE_SOURCE_DIR}/modules/memory/include)
	target_link_libraries(branch_test PRIVATE branch cas memory)
	add_test(NAME branch COMMAND branch_test)
endif()
