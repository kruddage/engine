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
		# Emscripten 6.0.2 flipped GROWABLE_ARRAYBUFFERS to default =1, which
		# backs the heap with a real resizable ArrayBuffer. Web APIs reject
		# views over a resizable buffer — Firefox's crypto.getRandomValues
		# (reached at startup via WASI random_get) throws "Argument 1 can't be
		# a resizable ArrayBuffer or ArrayBufferView". Opting out keeps memory
		# growth (copy into a fresh plain ArrayBuffer on grow), which the Web
		# Crypto API accepts.
		-sGROWABLE_ARRAYBUFFERS=0
		# mimalloc is the one allocator: libc, FETCH, the dynamic linker, every
		# side module, and modules/memory (which allocates through libc) all
		# share this single heap. Previously libc used emmalloc while the engine
		# used a separately-linked mimalloc — two allocators over one growable
		# heap, which corrupted on growth.
		-sMALLOC=mimalloc
		-sMAIN_MODULE=1
		-sFETCH=1
		--extern-pre-js ${CMAKE_CURRENT_SOURCE_DIR}/error_overlay.js
		--shell-file ${CMAKE_CURRENT_BINARY_DIR}/shell.html
		-sMAX_WEBGL_VERSION=2
		-sEXPORTED_FUNCTIONS=_main
	)
endif()
