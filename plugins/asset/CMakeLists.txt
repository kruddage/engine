add_library(asset_plugin asset_plugin.c)
target_include_directories(asset_plugin PUBLIC .
	${CMAKE_SOURCE_DIR}/plugins/include)

target_link_libraries(asset_plugin PRIVATE log memory subsystem subsystem_manager)

if(NOT EMSCRIPTEN)
	add_executable(asset_test asset_test.c)
	target_link_libraries(asset_test PRIVATE asset_plugin log memory)
	add_test(NAME asset COMMAND asset_test)

	add_executable(asset_codec_test asset_codec_test.c)
	target_link_libraries(asset_codec_test PRIVATE asset_plugin log memory)
	add_test(NAME asset_codec COMMAND asset_codec_test)

	add_executable(asset_api_test asset_api_test.c)
	target_link_libraries(asset_api_test PRIVATE asset_plugin log memory)
	add_test(NAME asset_api COMMAND asset_api_test)

	add_executable(asset_mut_test asset_mut_test.c)
	target_link_libraries(asset_mut_test PRIVATE asset_plugin log memory)
	add_test(NAME asset_mut COMMAND asset_mut_test)

	add_executable(asset_shader_test asset_shader_test.c)
	target_link_libraries(asset_shader_test PRIVATE asset_plugin log memory)
	add_test(NAME asset_shader COMMAND asset_shader_test)
endif()

if(EMSCRIPTEN)
	add_custom_command(
		OUTPUT ${CMAKE_BINARY_DIR}/asset_plugin.wasm
		COMMAND ${CMAKE_C_COMPILER}
			-sSIDE_MODULE=1
			-O2
			-I${CMAKE_CURRENT_SOURCE_DIR}
			-I${CMAKE_SOURCE_DIR}/modules/core/include
			-I${CMAKE_SOURCE_DIR}/plugins/include
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
