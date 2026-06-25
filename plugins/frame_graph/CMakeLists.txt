add_library(frame_graph fg.c)
target_include_directories(frame_graph PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(frame_graph PRIVATE
	${CMAKE_SOURCE_DIR}/plugins/renderer
	${CMAKE_SOURCE_DIR}/modules/core
	${CMAKE_SOURCE_DIR}/modules/log
)
target_link_libraries(frame_graph PRIVATE log subsystem subsystem_manager)

if(EMSCRIPTEN)
	add_custom_command(
		OUTPUT ${CMAKE_BINARY_DIR}/frame_graph.wasm
		COMMAND ${CMAKE_C_COMPILER}
			-sSIDE_MODULE=1
			-O2
			-I${CMAKE_CURRENT_SOURCE_DIR}
			-I${CMAKE_SOURCE_DIR}/plugins/renderer
			-I${CMAKE_SOURCE_DIR}/modules/core
			-I${CMAKE_SOURCE_DIR}/plugins/include
			-o ${CMAKE_BINARY_DIR}/frame_graph.wasm
			${CMAKE_CURRENT_SOURCE_DIR}/fg.c
		DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/fg.c
		COMMENT "Building frame_graph SIDE_MODULE"
	)
	add_custom_target(frame_graph_wasm ALL
		DEPENDS ${CMAKE_BINARY_DIR}/frame_graph.wasm
	)
	add_dependencies(index frame_graph_wasm)
endif()
