# Scheme runtime (#271) — vendored s7 built both ways the engine builds:
# a native static lib for ctest, and a WASM SIDE_MODULE for the browser.
#
# s7.c is third-party (0BSD, see third_party/VENDOR.md). It is compiled with
# -w so the engine's -Wall -Werror -Wpedantic is not enforced against upstream
# code, and with the feature defines documented in VENDOR.md.
set(S7_DEFS WITH_C_LOADER=0 WITH_MAIN=0)

if(NOT EMSCRIPTEN)
	add_library(s7 STATIC third_party/s7.c)
	target_include_directories(s7 PUBLIC third_party)
	target_compile_definitions(s7 PRIVATE ${S7_DEFS})
	target_compile_options(s7 PRIVATE -w)
	target_link_libraries(s7 PUBLIC m ${CMAKE_DL_LIBS})

	add_library(scheme_plugin scheme.c)
	target_include_directories(scheme_plugin PUBLIC .
		${CMAKE_SOURCE_DIR}/modules/core/include)
	target_link_libraries(scheme_plugin PRIVATE s7 subsystem_manager)

	add_executable(scheme_test scheme_test.c)
	target_link_libraries(scheme_test PRIVATE
		scheme_plugin s7 subsystem_manager)
	add_test(NAME scheme COMMAND scheme_test)
endif()

if(EMSCRIPTEN)
	add_custom_command(
		OUTPUT ${CMAKE_BINARY_DIR}/scheme.wasm
		COMMAND ${CMAKE_C_COMPILER}
			-sSIDE_MODULE=1
			-O2
			-w
			-DWITH_C_LOADER=0
			-DWITH_MAIN=0
			-I${CMAKE_CURRENT_SOURCE_DIR}
			-I${CMAKE_CURRENT_SOURCE_DIR}/third_party
			-I${CMAKE_SOURCE_DIR}/modules/core/include
			-o ${CMAKE_BINARY_DIR}/scheme.wasm
			${CMAKE_CURRENT_SOURCE_DIR}/scheme.c
			${CMAKE_CURRENT_SOURCE_DIR}/third_party/s7.c
		DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/scheme.c
			${CMAKE_CURRENT_SOURCE_DIR}/third_party/s7.c
			${CMAKE_CURRENT_SOURCE_DIR}/scheme.h
		COMMENT "Building scheme SIDE_MODULE (s7 10.8)"
	)
	add_custom_target(scheme_wasm ALL
		DEPENDS ${CMAKE_BINARY_DIR}/scheme.wasm
	)
	add_dependencies(index scheme_wasm)
endif()
