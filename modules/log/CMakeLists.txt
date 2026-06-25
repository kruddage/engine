add_library(log log.c)
target_include_directories(log PUBLIC . ${CMAKE_SOURCE_DIR}/plugins/include)

if(NOT EMSCRIPTEN)
	add_executable(log_test log_test.c)
	target_link_libraries(log_test PRIVATE log)
	add_test(NAME log COMMAND log_test)
endif()
