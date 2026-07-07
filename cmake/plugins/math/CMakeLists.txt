if(NOT EMSCRIPTEN)
	add_executable(math_test
		math_test.c
		math.c
		camera.c)
	target_include_directories(math_test PRIVATE
		${CMAKE_SOURCE_DIR}/plugins/include)
	target_link_libraries(math_test PRIVATE m)
	add_test(NAME math COMMAND math_test)
endif()
