add_library(edit_plugin edit.c edit_plugin.c)
target_include_directories(edit_plugin PUBLIC .
	${CMAKE_SOURCE_DIR}/plugins/include
	${CMAKE_SOURCE_DIR}/modules/core/include)
target_link_libraries(edit_plugin PRIVATE subsystem_manager)

if(NOT EMSCRIPTEN)
	# Link the pure history ops directly so the test needs no plugin glue.
	add_executable(edit_test edit_test.c edit.c)
	target_include_directories(edit_test PRIVATE .
		${CMAKE_SOURCE_DIR}/plugins/include)
	target_link_libraries(edit_test PRIVATE memory)
	add_test(NAME edit COMMAND edit_test)
endif()

if(EMSCRIPTEN)
	add_custom_command(
		OUTPUT ${CMAKE_BINARY_DIR}/edit_plugin.wasm
		COMMAND ${CMAKE_C_COMPILER}
			-sSIDE_MODULE=1
			-O2
			-I${CMAKE_CURRENT_SOURCE_DIR}
			-I${CMAKE_SOURCE_DIR}/modules/core/include
			-I${CMAKE_SOURCE_DIR}/plugins/include
			-o ${CMAKE_BINARY_DIR}/edit_plugin.wasm
			${CMAKE_CURRENT_SOURCE_DIR}/edit.c
			${CMAKE_CURRENT_SOURCE_DIR}/edit_plugin.c
		DEPENDS
			${CMAKE_CURRENT_SOURCE_DIR}/edit.c
			${CMAKE_CURRENT_SOURCE_DIR}/edit_plugin.c
		COMMENT "Building edit_plugin SIDE_MODULE"
	)
	add_custom_target(edit_plugin_wasm ALL
		DEPENDS ${CMAKE_BINARY_DIR}/edit_plugin.wasm
	)
	add_dependencies(index edit_plugin_wasm)
endif()
