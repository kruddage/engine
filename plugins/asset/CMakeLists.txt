add_library(asset_plugin asset_plugin.c)
target_include_directories(asset_plugin PUBLIC .)
target_include_directories(asset_plugin PRIVATE
	${CMAKE_SOURCE_DIR}/modules/core
	${CMAKE_SOURCE_DIR}/modules/log
	${CMAKE_SOURCE_DIR}/modules/memory
)
target_link_libraries(asset_plugin PRIVATE log memory subsystem subsystem_manager)

if(EMSCRIPTEN)
	add_custom_command(
		OUTPUT ${CMAKE_BINARY_DIR}/asset_plugin.wasm
		COMMAND ${CMAKE_C_COMPILER}
			-sSIDE_MODULE=1
			-O2
			-I${CMAKE_CURRENT_SOURCE_DIR}
			-I${CMAKE_SOURCE_DIR}/modules/core
			-I${CMAKE_SOURCE_DIR}/modules/log
			-I${CMAKE_SOURCE_DIR}/modules/memory
			-o ${CMAKE_BINARY_DIR}/asset_plugin.wasm
			${CMAKE_CURRENT_SOURCE_DIR}/asset_plugin.c
		DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/asset_plugin.c
		COMMENT "Building asset_plugin SIDE_MODULE"
	)
	add_custom_target(asset_plugin_wasm ALL
		DEPENDS ${CMAKE_BINARY_DIR}/asset_plugin.wasm
	)
	add_dependencies(index asset_plugin_wasm)
endif()
