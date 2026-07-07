if(EMSCRIPTEN)
	add_custom_command(
		OUTPUT ${CMAKE_BINARY_DIR}/imgui_plugin.wasm
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
			-o ${CMAKE_BINARY_DIR}/imgui_plugin.wasm
			${CMAKE_CURRENT_SOURCE_DIR}/imgui_plugin.cpp
			${imgui_SOURCE_DIR}/imgui.cpp
			${imgui_SOURCE_DIR}/imgui_draw.cpp
			${imgui_SOURCE_DIR}/imgui_tables.cpp
			${imgui_SOURCE_DIR}/imgui_widgets.cpp
			${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
		DEPENDS
			${CMAKE_CURRENT_SOURCE_DIR}/imgui_plugin.cpp
			${imgui_SOURCE_DIR}/imgui.cpp
			${imgui_SOURCE_DIR}/imgui_draw.cpp
			${imgui_SOURCE_DIR}/imgui_tables.cpp
			${imgui_SOURCE_DIR}/imgui_widgets.cpp
			${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
		COMMENT "Building imgui_plugin SIDE_MODULE"
	)
	add_custom_target(imgui_plugin_wasm ALL
		DEPENDS ${CMAKE_BINARY_DIR}/imgui_plugin.wasm
	)
	add_dependencies(index imgui_plugin_wasm)
endif()
