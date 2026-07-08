; SPDX-License-Identifier: GPL-2.0-or-later
;
; In-browser tabbed authoring surface + markdown parser. The side module reuses
; the same shape every other plugin's WASM build uses. imgui is fetched into
; ${imgui}, a path this spec doesn't own, so it passes through (raw ...).
((side-module "kruddboard"
	(compiler cxx)
	(flags "--std=c++17" "-fno-exceptions" "-fno-rtti")
	(includes (current) (raw "${generated}")
		(root "modules/core/include") (root "plugins/include")
		(raw "${imgui}") (raw "${imgui}/backends"))
	(sources (current "kruddboard.cpp") (current "md_parse.c"))
	(depends (current "kruddboard.cpp") (current "md_parse.c")
		(current "md_draw.h")
		(raw "${generated}/md_parse.h")))

 (native-only
	;; The C parser, still compiled into the WASM side module above and kept
	;; under its own test until the Scheme port takes over the browser too.
	(library "md_parse"
		(sources "md_parse.c")
		(public (current) (raw "${generated}")))
	(executable "md_parse_test"
		(sources "md_parse_test.c")
		(link "md_parse"))
	(test "md_parse" "md_parse_test")

	;; The Scheme port: krudd/build/modules/md_parse.scm runs inside the s7
	;; runtime, reached through the generated ${generated}/md_parse.scm.c shim
	;; (krudd's binding generator emits it from the module's ABI declaration; it
	;; exports the same md_parse ABI). ${generated} carries both that shim and
	;; the generated md_parse.h it and the test include, so it is a public
	;; include of this library. ../../third_party is s7.h; linking script drags
	;; the interpreter in. Running the exact same md_parse_test.c against it
	;; proves the two parsers byte-for-byte equal.
	(library "md_parse_scheme"
		(sources (raw "${generated}/md_parse.scm.c"))
		(public (raw "${generated}"))
		(private (root "modules/core/include")
			(raw "../../third_party"))
		(link "script"))
	(executable "md_parse_scheme_test"
		(sources "md_parse_test.c")
		(link "md_parse_scheme"))
	(test "md_parse_scheme" "md_parse_scheme_test")))
