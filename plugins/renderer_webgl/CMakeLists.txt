add_library(renderer_webgl renderer_webgl.c)
target_include_directories(renderer_webgl PRIVATE
	${CMAKE_SOURCE_DIR}/plugins/renderer
)
target_link_libraries(renderer_webgl PRIVATE log memory subsystem subsystem_manager)

if(EMSCRIPTEN)
	add_custom_command(
		OUTPUT ${CMAKE_BINARY_DIR}/renderer_webgl.wasm
		COMMAND ${CMAKE_C_COMPILER}
			-sSIDE_MODULE=1
			-O2
			-I${CMAKE_CURRENT_SOURCE_DIR}
			-I${CMAKE_SOURCE_DIR}/plugins/renderer
			-I${CMAKE_SOURCE_DIR}/modules/core/include
			-I${CMAKE_SOURCE_DIR}/plugins/include
			-o ${CMAKE_BINARY_DIR}/renderer_webgl.wasm
			${CMAKE_CURRENT_SOURCE_DIR}/renderer_webgl.c
		DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/renderer_webgl.c
		COMMENT "Building renderer_webgl SIDE_MODULE"
	)
	add_custom_target(renderer_webgl_wasm ALL
		DEPENDS ${CMAKE_BINARY_DIR}/renderer_webgl.wasm
	)
	add_dependencies(index renderer_webgl_wasm)
endif()
