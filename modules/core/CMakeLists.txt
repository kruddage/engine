configure_file(version.h.in version.h)
configure_file(shell.html.in ${CMAKE_CURRENT_BINARY_DIR}/shell.html @ONLY)

add_library(subsystem subsystem.c)
target_include_directories(subsystem PUBLIC include)

add_library(subsystem_manager subsystem_manager.c)
target_include_directories(subsystem_manager PUBLIC include)

add_library(plugin_loader plugin_loader.c)
target_include_directories(plugin_loader PUBLIC include)
target_include_directories(plugin_loader PRIVATE
	${CMAKE_SOURCE_DIR}/modules/log/include
	${CMAKE_SOURCE_DIR}/plugins/include
)

add_executable(index engine.c)
target_include_directories(index PRIVATE include ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_SOURCE_DIR}/plugins/include)
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
	target_sources(index PRIVATE plugin_abi.c)
	set_target_properties(index PROPERTIES
		SUFFIX ".html"
		RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
		LINKER_LANGUAGE CXX
	)
	target_link_options(index PRIVATE
		-sENVIRONMENT=web
		-sALLOW_MEMORY_GROWTH=1
		# No Emscripten allocator: modules/memory/mi_libc_shim.c supplies the
		# libc malloc family on top of mimalloc, so libc and the engine's
		# memory_api share one heap. Two allocators over one growable heap
		# corrupted on growth.
		-sMALLOC=none
		-sMAIN_MODULE=1
		-sFETCH=1
		--extern-pre-js ${CMAKE_CURRENT_SOURCE_DIR}/error_overlay.js
		--shell-file ${CMAKE_CURRENT_BINARY_DIR}/shell.html
		-sMAX_WEBGL_VERSION=2
		# MAIN_MODULE=1 exports every symbol (so side modules resolve malloc/
		# free from the shim via the dynamic linker); listing more here is
		# ignored with a warning.
		-sEXPORTED_FUNCTIONS=_main
	)
endif()
