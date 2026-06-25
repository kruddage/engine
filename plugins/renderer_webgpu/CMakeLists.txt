if(EMSCRIPTEN)
	add_custom_command(
		OUTPUT ${CMAKE_BINARY_DIR}/renderer_webgpu.wasm
		COMMAND ${CMAKE_C_COMPILER}
			-sSIDE_MODULE=1
			--use-port=emdawnwebgpu
			-O2
			-I${CMAKE_CURRENT_SOURCE_DIR}
			-I${CMAKE_SOURCE_DIR}/plugins/renderer
			-I${CMAKE_SOURCE_DIR}/modules/core
			-I${CMAKE_SOURCE_DIR}/modules/log
			-o ${CMAKE_BINARY_DIR}/renderer_webgpu.wasm
			${CMAKE_CURRENT_SOURCE_DIR}/renderer_webgpu.c
		DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/renderer_webgpu.c
		COMMENT "Building renderer_webgpu SIDE_MODULE"
	)
	add_custom_target(renderer_webgpu_wasm ALL
		DEPENDS ${CMAKE_BINARY_DIR}/renderer_webgpu.wasm
	)
	add_dependencies(index renderer_webgpu_wasm)
endif()
