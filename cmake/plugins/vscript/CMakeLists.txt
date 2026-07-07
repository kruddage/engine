add_library(vscript_plugin vscript.c)
target_include_directories(vscript_plugin PUBLIC .
	${CMAKE_SOURCE_DIR}/plugins/include
	${CMAKE_SOURCE_DIR}/modules/core/include)
target_link_libraries(vscript_plugin PRIVATE memory subsystem_manager)

if(NOT EMSCRIPTEN)
	add_executable(vscript_test vscript_test.c)
	target_link_libraries(vscript_test PRIVATE
		vscript_plugin asset_plugin log memory subsystem_manager)
	add_test(NAME vscript COMMAND vscript_test)
endif()

if(EMSCRIPTEN)
	add_custom_command(
		OUTPUT ${CMAKE_BINARY_DIR}/vscript.wasm
		COMMAND ${CMAKE_C_COMPILER}
			-sSIDE_MODULE=1
			-O2
			-I${CMAKE_CURRENT_SOURCE_DIR}
			-I${CMAKE_SOURCE_DIR}/modules/core/include
			-I${CMAKE_SOURCE_DIR}/plugins/include
			-o ${CMAKE_BINARY_DIR}/vscript.wasm
			${CMAKE_CURRENT_SOURCE_DIR}/vscript.c
		DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/vscript.c
		COMMENT "Building vscript SIDE_MODULE"
	)
	add_custom_target(vscript_wasm ALL
		DEPENDS ${CMAKE_BINARY_DIR}/vscript.wasm
	)
	add_dependencies(index vscript_wasm)
endif()
