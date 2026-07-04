if(NOT EMSCRIPTEN)
	add_executable(scene_renderer_test
		scene_renderer_test.c
		scene_renderer.c
		${CMAKE_SOURCE_DIR}/plugins/math/math.c
		${CMAKE_SOURCE_DIR}/plugins/math/camera.c
		${CMAKE_SOURCE_DIR}/plugins/asset/primitives.c)
	target_include_directories(scene_renderer_test PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${CMAKE_SOURCE_DIR}/plugins/renderer
		${CMAKE_SOURCE_DIR}/plugins/renderer_null
		${CMAKE_SOURCE_DIR}/plugins/frame_graph
		${CMAKE_SOURCE_DIR}/plugins/asset
		${CMAKE_SOURCE_DIR}/plugins/include)
	target_link_libraries(scene_renderer_test PRIVATE
		frame_graph renderer_null log memory subsystem_manager m)
	add_test(NAME scene_renderer COMMAND scene_renderer_test)
endif()

if(EMSCRIPTEN)
	add_custom_command(
		OUTPUT ${CMAKE_BINARY_DIR}/scene_renderer.wasm
		COMMAND ${CMAKE_C_COMPILER}
			-sSIDE_MODULE=1
			-O2
			-I${CMAKE_CURRENT_SOURCE_DIR}
			-I${CMAKE_SOURCE_DIR}/plugins/renderer
			-I${CMAKE_SOURCE_DIR}/plugins/frame_graph
			-I${CMAKE_SOURCE_DIR}/modules/core/include
			-I${CMAKE_SOURCE_DIR}/plugins/include
			-o ${CMAKE_BINARY_DIR}/scene_renderer.wasm
			${CMAKE_CURRENT_SOURCE_DIR}/scene_renderer.c
			${CMAKE_SOURCE_DIR}/plugins/math/math.c
			${CMAKE_SOURCE_DIR}/plugins/math/camera.c
		DEPENDS
			${CMAKE_CURRENT_SOURCE_DIR}/scene_renderer.c
			${CMAKE_SOURCE_DIR}/plugins/math/math.c
			${CMAKE_SOURCE_DIR}/plugins/math/camera.c
			${CMAKE_SOURCE_DIR}/plugins/include/mesh.h
		COMMENT "Building scene_renderer SIDE_MODULE"
	)
	add_custom_target(scene_renderer_wasm ALL
		DEPENDS ${CMAKE_BINARY_DIR}/scene_renderer.wasm
	)
	add_dependencies(index scene_renderer_wasm)
endif()
