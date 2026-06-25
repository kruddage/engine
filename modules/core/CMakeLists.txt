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
	target_link_options(index PRIVATE
		-sENVIRONMENT=web
		-sALLOW_MEMORY_GROWTH=1
		-sMALLOC=emmalloc
		-sMAIN_MODULE=1
		-sFETCH=1
		--use-port=emdawnwebgpu
	)
endif()
