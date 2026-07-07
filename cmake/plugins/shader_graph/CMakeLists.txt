add_library(shader_graph_plugin
	shader_nodes.c glsl_backend.c shader_graph.c)
target_include_directories(shader_graph_plugin PUBLIC .
	${CMAKE_SOURCE_DIR}/plugins/include
	${CMAKE_SOURCE_DIR}/modules/core/include)
target_link_libraries(shader_graph_plugin PRIVATE memory subsystem_manager)

if(NOT EMSCRIPTEN)
	add_executable(shader_graph_test shader_graph_test.c)
	target_link_libraries(shader_graph_test PRIVATE
		shader_graph_plugin vscript_plugin asset_plugin
		log memory subsystem_manager)
	add_test(NAME shader_graph COMMAND shader_graph_test)
endif()

if(EMSCRIPTEN)
	add_custom_command(
		OUTPUT ${CMAKE_BINARY_DIR}/shader_graph.wasm
		COMMAND ${CMAKE_C_COMPILER}
			-sSIDE_MODULE=1
			-O2
			-I${CMAKE_CURRENT_SOURCE_DIR}
			-I${CMAKE_SOURCE_DIR}/modules/core/include
			-I${CMAKE_SOURCE_DIR}/plugins/include
			-o ${CMAKE_BINARY_DIR}/shader_graph.wasm
			${CMAKE_CURRENT_SOURCE_DIR}/shader_nodes.c
			${CMAKE_CURRENT_SOURCE_DIR}/glsl_backend.c
			${CMAKE_CURRENT_SOURCE_DIR}/shader_graph.c
		DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shader_nodes.c
			${CMAKE_CURRENT_SOURCE_DIR}/glsl_backend.c
			${CMAKE_CURRENT_SOURCE_DIR}/shader_graph.c
		COMMENT "Building shader_graph SIDE_MODULE"
	)
	add_custom_target(shader_graph_wasm ALL
		DEPENDS ${CMAKE_BINARY_DIR}/shader_graph.wasm
	)
	add_dependencies(index shader_graph_wasm)
endif()
