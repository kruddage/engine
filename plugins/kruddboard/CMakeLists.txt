if(EMSCRIPTEN)
	add_custom_command(
		OUTPUT ${CMAKE_BINARY_DIR}/kruddboard.wasm
		COMMAND ${CMAKE_CXX_COMPILER}
			-sSIDE_MODULE=1
			-O2
			--std=c++17
			-fno-exceptions
			-fno-rtti
			-I${CMAKE_CURRENT_SOURCE_DIR}
			-I${CMAKE_SOURCE_DIR}/modules/core/include
			-I${CMAKE_SOURCE_DIR}/plugins/include
			-I${imgui_SOURCE_DIR}
			-I${imgui_SOURCE_DIR}/backends
			-o ${CMAKE_BINARY_DIR}/kruddboard.wasm
			${CMAKE_CURRENT_SOURCE_DIR}/kruddboard.cpp
		DEPENDS
			${CMAKE_CURRENT_SOURCE_DIR}/kruddboard.cpp
		COMMENT "Building kruddboard SIDE_MODULE"
	)
	add_custom_target(kruddboard_wasm ALL
		DEPENDS ${CMAKE_BINARY_DIR}/kruddboard.wasm
	)
	add_dependencies(index kruddboard_wasm)
endif()

if(NOT EMSCRIPTEN)
	add_library(md_parse md_parse.c)
	target_include_directories(md_parse PUBLIC
		${CMAKE_CURRENT_SOURCE_DIR})

	add_executable(md_parse_test md_parse_test.c)
	target_link_libraries(md_parse_test PRIVATE md_parse)
	add_test(NAME md_parse COMMAND md_parse_test)
endif()
