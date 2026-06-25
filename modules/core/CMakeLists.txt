add_library(subsystem subsystem.c)
target_include_directories(subsystem PUBLIC .)

add_library(subsystem_manager subsystem_manager.c)
target_include_directories(subsystem_manager PUBLIC .)

add_library(plugin_loader plugin_loader.c)
target_include_directories(plugin_loader PUBLIC .)
target_include_directories(plugin_loader PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../log)

add_executable(index engine.c)
target_include_directories(index PRIVATE .)
target_link_libraries(index PRIVATE subsystem subsystem_manager log memory plugin_loader)

if(EMSCRIPTEN)
	set_target_properties(index PROPERTIES
		SUFFIX ".html"
		RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
	)
	# Functions called only from side modules (plugins) are not reachable from
	# the main module's own code and can be dead-code-eliminated before the
	# dynamic linker can export them.  List them explicitly so they survive.
	set(PLUGIN_API_EXPORTS
		_main
		_subsystem_manager_register
		_subsystem_manager_get_api
		_mem_alloc
		_mem_alloc_zero
		_mem_free
		_mem_pool_create
		_mem_pool_alloc
		_mem_pool_free
		_mem_pool_destroy
	)
	list(JOIN PLUGIN_API_EXPORTS "," PLUGIN_API_EXPORTS_STR)
	target_link_options(index PRIVATE
		-sENVIRONMENT=web
		-sALLOW_MEMORY_GROWTH=1
		-sMALLOC=emmalloc
		-sMAIN_MODULE=1
		-sFETCH=1
		--use-port=emdawnwebgpu
		--extern-pre-js ${CMAKE_CURRENT_SOURCE_DIR}/error_overlay.js
		-sEXPORT_ALL=1
		-sEXPORTED_FUNCTIONS=${PLUGIN_API_EXPORTS_STR}
	)
endif()
