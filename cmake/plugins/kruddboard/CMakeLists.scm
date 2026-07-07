; SPDX-License-Identifier: GPL-2.0-or-later
;
; In-browser tabbed authoring surface + markdown parser. The CHANGELOG.md ->
; header embed is a one-off custom_command with no form of its own yet, so it
; stays (verbatim ...); the side module build reuses the same shape every
; other plugin's WASM build uses. imgui itself is fetched by the root
; CMakeLists.txt (FetchContent); ${imgui_SOURCE_DIR} is a CMake variable this
; spec doesn't own, so its paths pass through (raw ...).
((verbatim "if(EMSCRIPTEN)
	# Bake CHANGELOG.md into a generated header the \"What's New\" tab renders
	# through the md_parse/md_draw stack — no runtime fetch, no new ABI.
	set(_changelog_hdr ${CMAKE_BINARY_DIR}/generated/changelog_data.h)
	add_custom_command(
		OUTPUT ${_changelog_hdr}
		COMMAND ${CMAKE_COMMAND}
			-DINPUT=${KRUDD_REPO_ROOT}/CHANGELOG.md
			-DOUTPUT=${_changelog_hdr}
			-DSYMBOL=CHANGELOG_MD
			-P ${CMAKE_CURRENT_SOURCE_DIR}/embed_changelog.cmake
		DEPENDS
			${KRUDD_REPO_ROOT}/CHANGELOG.md
			${CMAKE_CURRENT_SOURCE_DIR}/embed_changelog.cmake
		COMMENT \"Embedding CHANGELOG.md -> changelog_data.h\"
		VERBATIM
	)
endif()")

 (side-module "kruddboard"
	(compiler cxx)
	(flags "--std=c++17" "-fno-exceptions" "-fno-rtti")
	(includes (current) (raw "${CMAKE_BINARY_DIR}/generated")
		(root "modules/core/include") (root "plugins/include")
		(raw "${imgui_SOURCE_DIR}") (raw "${imgui_SOURCE_DIR}/backends"))
	(sources (current "kruddboard.cpp") (current "md_parse.c"))
	(depends (current "kruddboard.cpp") (current "md_parse.c")
		(current "md_parse.h") (current "md_draw.h")
		(raw "${CMAKE_BINARY_DIR}/generated/changelog_data.h")))

 (native-only
	(library "md_parse"
		(sources "md_parse.c")
		(public (current)))
	(executable "md_parse_test"
		(sources "md_parse_test.c")
		(link "md_parse"))
	(test "md_parse" "md_parse_test")))
