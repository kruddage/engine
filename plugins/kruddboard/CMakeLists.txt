if(EMSCRIPTEN)
	# Bake CHANGELOG.md into a generated header the "What's New" tab renders
	# through the md_parse/md_draw stack — no runtime fetch, no new ABI.
	set(_changelog_hdr ${CMAKE_BINARY_DIR}/generated/changelog_data.h)
	add_custom_command(
		OUTPUT ${_changelog_hdr}
		COMMAND ${CMAKE_COMMAND}
			-DINPUT=${CMAKE_SOURCE_DIR}/CHANGELOG.md
			-DOUTPUT=${_changelog_hdr}
			-DSYMBOL=CHANGELOG_MD
			-P ${CMAKE_CURRENT_SOURCE_DIR}/embed_changelog.cmake
		DEPENDS
			${CMAKE_SOURCE_DIR}/CHANGELOG.md
			${CMAKE_CURRENT_SOURCE_DIR}/embed_changelog.cmake
		COMMENT "Embedding CHANGELOG.md -> changelog_data.h"
		VERBATIM
	)

	add_custom_command(
		OUTPUT ${CMAKE_BINARY_DIR}/kruddboard.wasm
		COMMAND ${CMAKE_CXX_COMPILER}
			-sSIDE_MODULE=1
			-O2
			--std=c++17
			-fno-exceptions
			-fno-rtti
			-I${CMAKE_CURRENT_SOURCE_DIR}
			-I${CMAKE_BINARY_DIR}/generated
			-I${CMAKE_SOURCE_DIR}/modules/core/include
			-I${CMAKE_SOURCE_DIR}/plugins/include
			-I${imgui_SOURCE_DIR}
			-I${imgui_SOURCE_DIR}/backends
			-o ${CMAKE_BINARY_DIR}/kruddboard.wasm
			${CMAKE_CURRENT_SOURCE_DIR}/kruddboard.cpp
			${CMAKE_CURRENT_SOURCE_DIR}/md_parse.c
		DEPENDS
			${CMAKE_CURRENT_SOURCE_DIR}/kruddboard.cpp
			${CMAKE_CURRENT_SOURCE_DIR}/md_parse.c
			${CMAKE_CURRENT_SOURCE_DIR}/md_parse.h
			${CMAKE_CURRENT_SOURCE_DIR}/md_draw.h
			${_changelog_hdr}
		COMMENT "Building kruddboard SIDE_MODULE"
	)
	add_custom_target(kruddboard_wasm ALL
		DEPENDS ${CMAKE_BINARY_DIR}/kruddboard.wasm
	)
	add_dependencies(index kruddboard_wasm)
endif()

if(NOT EMSCRIPTEN)
	add_library(md_parse md_parse.c)
	target_include_directories(md_parse PUBLIC
		${CMAKE_CURRENT_SOURCE_DIR})

	add_executable(md_parse_test md_parse_test.c)
	target_link_libraries(md_parse_test PRIVATE md_parse)
	add_test(NAME md_parse COMMAND md_parse_test)
endif()
