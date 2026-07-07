; SPDX-License-Identifier: GPL-2.0-or-later
;
; In-browser tabbed authoring surface + markdown parser. The side module reuses
; the same shape every other plugin's WASM build uses. CHANGELOG.md is baked
; into generated/changelog_data.h at synthesis time by krudd/introspect.scm's
; changelog codegen (the "What's New" tab renders it through the md_parse/md_draw
; stack — no runtime fetch, no new ABI); the side module just depends on the
; generated header. imgui is fetched into ${imgui}, a path this spec doesn't own,
; so it passes through (raw ...).
((side-module "kruddboard"
	(compiler cxx)
	(flags "--std=c++17" "-fno-exceptions" "-fno-rtti")
	(includes (current) (raw "${generated}")
		(root "modules/core/include") (root "plugins/include")
		(raw "${imgui}") (raw "${imgui}/backends"))
	(sources (current "kruddboard.cpp") (current "md_parse.c"))
	(depends (current "kruddboard.cpp") (current "md_parse.c")
		(current "md_parse.h") (current "md_draw.h")
		(raw "${generated}/changelog_data.h")))

 (native-only
	(library "md_parse"
		(sources "md_parse.c")
		(public (current)))
	(executable "md_parse_test"
		(sources "md_parse_test.c")
		(link "md_parse"))
	(test "md_parse" "md_parse_test")))
