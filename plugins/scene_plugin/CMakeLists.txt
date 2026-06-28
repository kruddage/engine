add_library(scene_plugin scene_plugin.c)
target_include_directories(scene_plugin PUBLIC .
	${CMAKE_SOURCE_DIR}/plugins/include
	${CMAKE_SOURCE_DIR}/modules/core/include)
target_link_libraries(scene_plugin PRIVATE memory subsystem_manager)

if(NOT EMSCRIPTEN)
	add_executable(scene_test scene_test.c)
	target_link_libraries(scene_test PRIVATE
		scene_plugin asset_plugin log memory subsystem_manager)
	add_test(NAME scene COMMAND scene_test)
endif()

if(EMSCRIPTEN)
	add_custom_command(
		OUTPUT ${CMAKE_BINARY_DIR}/scene_plugin.wasm
		COMMAND ${CMAKE_C_COMPILER}
			-sSIDE_MODULE=1
			-O2
			-I${CMAKE_CURRENT_SOURCE_DIR}
			-I${CMAKE_SOURCE_DIR}/modules/core/include
			-I${CMAKE_SOURCE_DIR}/plugins/include
			-o ${CMAKE_BINARY_DIR}/scene_plugin.wasm
			${CMAKE_CURRENT_SOURCE_DIR}/scene_plugin.c
		DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/scene_plugin.c
		COMMENT "Building scene_plugin SIDE_MODULE"
	)
	add_custom_target(scene_plugin_wasm ALL
		DEPENDS ${CMAKE_BINARY_DIR}/scene_plugin.wasm
	)
	add_dependencies(index scene_plugin_wasm)
endif()
