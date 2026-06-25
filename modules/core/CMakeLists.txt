add_library(subsystem subsystem.c)
target_include_directories(subsystem PUBLIC .)

add_library(subsystem_manager subsystem_manager.c)
target_include_directories(subsystem_manager PUBLIC .)

add_library(plugin_loader plugin_loader.c)
target_include_directories(plugin_loader PUBLIC .)
target_include_directories(plugin_loader PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/../log
	${CMAKE_SOURCE_DIR}/plugins/include
)

add_executable(index engine.c)
target_include_directories(index PRIVATE . ${CMAKE_SOURCE_DIR}/plugins/include)
target_link_libraries(index PRIVATE subsystem subsystem_manager log memory plugin_loader)

if(NOT EMSCRIPTEN)
	add_executable(subsystem_test subsystem_test.c)
	target_link_libraries(subsystem_test PRIVATE subsystem)
	add_test(NAME subsystem COMMAND subsystem_test)

	add_executable(subsystem_manager_test subsystem_manager_test.c)
	target_link_libraries(subsystem_manager_test PRIVATE subsystem_manager)
	add_test(NAME subsystem_manager COMMAND subsystem_manager_test)

	add_executable(plugin_loader_test plugin_loader_test.c)
	target_link_libraries(plugin_loader_test PRIVATE plugin_loader)
	add_test(NAME plugin_loader COMMAND plugin_loader_test)
endif()

if(EMSCRIPTEN)
	set_target_properties(index PROPERTIES
		SUFFIX ".html"
		RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
		LINKER_LANGUAGE CXX
	)
	target_link_options(index PRIVATE
		-sENVIRONMENT=web
		-sALLOW_MEMORY_GROWTH=1
		-sMALLOC=emmalloc
		-sMAIN_MODULE=1
		-sFETCH=1
		--extern-pre-js ${CMAKE_CURRENT_SOURCE_DIR}/error_overlay.js
		--shell-file ${CMAKE_CURRENT_SOURCE_DIR}/shell.html
		-sMAX_WEBGL_VERSION=2
		-sEXPORTED_FUNCTIONS=_main
	)
endif()
