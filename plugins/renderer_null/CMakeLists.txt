add_library(renderer_null renderer_null.c)
target_include_directories(renderer_null PRIVATE
	${CMAKE_SOURCE_DIR}/plugins/renderer
	${CMAKE_SOURCE_DIR}/modules/core
	${CMAKE_SOURCE_DIR}/modules/log
)
target_link_libraries(renderer_null PRIVATE log subsystem subsystem_manager)

if(EMSCRIPTEN)
	add_custom_command(
		OUTPUT ${CMAKE_BINARY_DIR}/renderer_null.wasm
		COMMAND ${CMAKE_C_COMPILER}
			-sSIDE_MODULE=1
			-O2
			-I${CMAKE_CURRENT_SOURCE_DIR}
			-I${CMAKE_SOURCE_DIR}/plugins/renderer
			-I${CMAKE_SOURCE_DIR}/modules/core
			-I${CMAKE_SOURCE_DIR}/plugins/include
			-o ${CMAKE_BINARY_DIR}/renderer_null.wasm
			${CMAKE_CURRENT_SOURCE_DIR}/renderer_null.c
		DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/renderer_null.c
		COMMENT "Building renderer_null SIDE_MODULE"
	)
	add_custom_target(renderer_null_wasm ALL
		DEPENDS ${CMAKE_BINARY_DIR}/renderer_null.wasm
	)
	add_dependencies(index renderer_null_wasm)
endif()
