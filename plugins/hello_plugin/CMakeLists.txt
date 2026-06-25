if(EMSCRIPTEN)
	add_custom_command(
		OUTPUT ${CMAKE_BINARY_DIR}/hello_plugin.wasm
		COMMAND ${CMAKE_C_COMPILER}
			-sSIDE_MODULE=1
			-O2
			-I${CMAKE_SOURCE_DIR}/modules/core
			-I${CMAKE_SOURCE_DIR}/modules/log
			-o ${CMAKE_BINARY_DIR}/hello_plugin.wasm
			${CMAKE_CURRENT_SOURCE_DIR}/hello_plugin.c
		DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/hello_plugin.c
		COMMENT "Building hello_plugin SIDE_MODULE"
	)
	add_custom_target(hello_plugin ALL
		DEPENDS ${CMAKE_BINARY_DIR}/hello_plugin.wasm
	)
	add_dependencies(index hello_plugin)
endif()
