add_executable(memory_test memory_test.c)
target_link_libraries(memory_test PRIVATE memory)
add_test(NAME memory COMMAND memory_test)

add_executable(log_test log_test.c)
target_link_libraries(log_test PRIVATE log)
add_test(NAME log COMMAND log_test)

add_executable(subsystem_test subsystem_test.c)
target_link_libraries(subsystem_test PRIVATE subsystem)
add_test(NAME subsystem COMMAND subsystem_test)

add_executable(subsystem_manager_test subsystem_manager_test.c)
target_link_libraries(subsystem_manager_test PRIVATE subsystem_manager)
add_test(NAME subsystem_manager COMMAND subsystem_manager_test)

add_executable(asset_test asset_test.c)
target_link_libraries(asset_test PRIVATE asset_plugin log memory)
add_test(NAME asset COMMAND asset_test)

add_executable(plugin_loader_test plugin_loader_test.c)
target_include_directories(plugin_loader_test PRIVATE ${CMAKE_SOURCE_DIR}/modules/core)
target_link_libraries(plugin_loader_test PRIVATE plugin_loader)
add_test(NAME plugin_loader COMMAND plugin_loader_test)

add_executable(renderer_null_test renderer_null_test.c)
target_include_directories(renderer_null_test PRIVATE
	${CMAKE_SOURCE_DIR}/plugins/renderer
	${CMAKE_SOURCE_DIR}/plugins/renderer_null
	${CMAKE_SOURCE_DIR}/modules/core
)
target_link_libraries(renderer_null_test PRIVATE renderer_null log subsystem_manager)
add_test(NAME renderer_null COMMAND renderer_null_test)

add_executable(fg_test fg_test.c)
target_include_directories(fg_test PRIVATE
	${CMAKE_SOURCE_DIR}/plugins/renderer
	${CMAKE_SOURCE_DIR}/plugins/renderer_null
	${CMAKE_SOURCE_DIR}/plugins/frame_graph
	${CMAKE_SOURCE_DIR}/modules/core
)
target_link_libraries(fg_test PRIVATE frame_graph renderer_null log subsystem_manager)
add_test(NAME fg COMMAND fg_test)
