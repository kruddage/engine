; SPDX-License-Identifier: GPL-2.0-or-later
;
; ImGui debug UI shell — a side module only, no native test target. imgui
; itself is fetched by krudd (krudd/introspect.scm's krudd-fetch) into ${imgui},
; a path this spec doesn't own, so its paths pass through (raw ...).
((side-module "imgui_plugin"
	(compiler cxx)
	(flags "--std=c++17" "-fno-exceptions" "-fno-rtti")
	(includes (current) (root "modules/core/include")
		(root "plugins/include") (raw "${imgui}")
		(raw "${imgui}/backends"))
	(sources (current "imgui_plugin.cpp") (raw "${imgui}/imgui.cpp")
		(raw "${imgui}/imgui_draw.cpp")
		(raw "${imgui}/imgui_tables.cpp")
		(raw "${imgui}/imgui_widgets.cpp")
		(raw "${imgui}/backends/imgui_impl_opengl3.cpp"))
	(depends (current "imgui_plugin.cpp") (raw "${imgui}/imgui.cpp")
		(raw "${imgui}/imgui_draw.cpp")
		(raw "${imgui}/imgui_tables.cpp")
		(raw "${imgui}/imgui_widgets.cpp")
		(raw "${imgui}/backends/imgui_impl_opengl3.cpp"))))
