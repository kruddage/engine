add_library(backend_record backend_record.c)
target_include_directories(backend_record PUBLIC .
	${CMAKE_SOURCE_DIR}/plugins/include)

add_library(backend_plugin backend_plugin.c)
target_include_directories(backend_plugin PUBLIC .
	${CMAKE_SOURCE_DIR}/modules/core/include
	${CMAKE_SOURCE_DIR}/plugins/include)
target_link_libraries(backend_plugin PRIVATE backend_record log subsystem subsystem_manager)

if(NOT EMSCRIPTEN)
	add_executable(backend_record_test backend_record_test.c)
	target_link_libraries(backend_record_test PRIVATE backend_record)
	add_test(NAME backend_record COMMAND backend_record_test)
endif()

if(EMSCRIPTEN)
	add_custom_command(
		OUTPUT ${CMAKE_BINARY_DIR}/backend_plugin.wasm
		COMMAND ${CMAKE_C_COMPILER}
			-sSIDE_MODULE=1
			-O2
			-I${CMAKE_CURRENT_SOURCE_DIR}
			-I${CMAKE_SOURCE_DIR}/modules/core/include
			-I${CMAKE_SOURCE_DIR}/plugins/include
			-o ${CMAKE_BINARY_DIR}/backend_plugin.wasm
			${CMAKE_CURRENT_SOURCE_DIR}/backend_plugin.c
			${CMAKE_CURRENT_SOURCE_DIR}/backend_record.c
		DEPENDS
			${CMAKE_CURRENT_SOURCE_DIR}/backend_plugin.c
			${CMAKE_CURRENT_SOURCE_DIR}/backend_record.c
			${CMAKE_CURRENT_SOURCE_DIR}/backend_record.h
			${CMAKE_CURRENT_SOURCE_DIR}/backend_api.h
		COMMENT "Building backend_plugin SIDE_MODULE"
	)
	add_custom_target(backend_plugin_wasm ALL
		DEPENDS ${CMAKE_BINARY_DIR}/backend_plugin.wasm
	)
	add_dependencies(index backend_plugin_wasm)
endif()
