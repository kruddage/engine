add_library(log
	log.c
	${CMAKE_SOURCE_DIR}/modules/core/ring_buf.c
)
target_include_directories(log PUBLIC include ${CMAKE_SOURCE_DIR}/plugins/include)
target_include_directories(log PRIVATE ${CMAKE_SOURCE_DIR}/modules/core/include)

if(NOT EMSCRIPTEN)
	add_executable(log_test log_test.c)
	target_link_libraries(log_test PRIVATE log)
	add_test(NAME log COMMAND log_test)
endif()
