add_library(entity_plugin entity.c entity_plugin.c scene_edit.c)
target_include_directories(entity_plugin PUBLIC .
	${CMAKE_SOURCE_DIR}/plugins/include
	${CMAKE_SOURCE_DIR}/modules/core/include)
target_link_libraries(entity_plugin PRIVATE memory subsystem_manager)

if(NOT EMSCRIPTEN)
	# Link the pure world ops directly so the test needs no plugin glue.
	add_executable(entity_test entity_test.c entity.c)
	target_include_directories(entity_test PRIVATE .
		${CMAKE_SOURCE_DIR}/plugins/include
		${CMAKE_SOURCE_DIR}/modules/core/include
		${CMAKE_SOURCE_DIR}/modules/memory/include)
	target_link_libraries(entity_test PRIVATE memory)
	add_test(NAME entity COMMAND entity_test)

	# Snapshot-based undo recording: link the world ops + the edit history
	# directly so the test drives the same command path as the plugin.
	add_executable(scene_edit_test scene_edit_test.c scene_edit.c entity.c
		${CMAKE_SOURCE_DIR}/plugins/edit_plugin/edit.c)
	target_include_directories(scene_edit_test PRIVATE .
		${CMAKE_SOURCE_DIR}/plugins/include
		${CMAKE_SOURCE_DIR}/plugins/edit_plugin
		${CMAKE_SOURCE_DIR}/modules/core/include
		${CMAKE_SOURCE_DIR}/modules/memory/include)
	target_link_libraries(scene_edit_test PRIVATE memory)
	add_test(NAME scene_edit COMMAND scene_edit_test)
endif()

if(EMSCRIPTEN)
	add_custom_command(
		OUTPUT ${CMAKE_BINARY_DIR}/entity_plugin.wasm
		COMMAND ${CMAKE_C_COMPILER}
			-sSIDE_MODULE=1
			-O2
			-I${CMAKE_CURRENT_SOURCE_DIR}
			-I${CMAKE_SOURCE_DIR}/modules/core/include
			-I${CMAKE_SOURCE_DIR}/plugins/include
			-o ${CMAKE_BINARY_DIR}/entity_plugin.wasm
			${CMAKE_CURRENT_SOURCE_DIR}/entity.c
			${CMAKE_CURRENT_SOURCE_DIR}/entity_plugin.c
			${CMAKE_CURRENT_SOURCE_DIR}/scene_edit.c
		DEPENDS
			${CMAKE_CURRENT_SOURCE_DIR}/entity.c
			${CMAKE_CURRENT_SOURCE_DIR}/entity_plugin.c
			${CMAKE_CURRENT_SOURCE_DIR}/scene_edit.c
		COMMENT "Building entity_plugin SIDE_MODULE"
	)
	add_custom_target(entity_plugin_wasm ALL
		DEPENDS ${CMAKE_BINARY_DIR}/entity_plugin.wasm
	)
	add_dependencies(index entity_plugin_wasm)
endif()
