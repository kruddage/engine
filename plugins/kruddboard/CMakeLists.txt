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
			-I${CMAKE_SOURCE_DIR}/modules/core
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
