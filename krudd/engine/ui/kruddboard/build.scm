; SPDX-License-Identifier: GPL-2.0-or-later
((wasm-only
	(library "kruddboard"
		(wasm-flags "--std=c++17" "-fno-exceptions" "-fno-rtti")
		(sources "kruddboard.cpp" (raw "${generated}/md_parse.scm.c"))
		(private "." (raw "${generated}") (raw "../third_party")
			(root "asset"))
		(link "mesh_script" "texture_script" "script" "log"
			"memory" "subsystem" "subsystem_manager")))

 (native-only
	(library "md_parse"
		(sources "md_parse.c")
		(public (current) (raw "${generated}")))
	(executable "md_parse_test"
		(sources "md_parse_test.c")
		(link "md_parse"))
	(test "md_parse" "md_parse_test")

	(library "md_parse_scheme"
		(sources (raw "${generated}/md_parse.scm.c"))
		(public (raw "${generated}"))
		(private (root "core/include")
			(raw "../third_party"))
		(link "script"))
	(executable "md_parse_scheme_test"
		(sources "md_parse_test.c")
		(link "md_parse_scheme"))
	(test "md_parse_scheme" "md_parse_scheme_test")))
