; SPDX-License-Identifier: GPL-2.0-or-later
;
; ImGui debug UI shell — a side module only, no native test target. imgui
; itself is fetched by the root CMakeLists.txt (FetchContent); ${imgui_SOURCE_DIR}
; is a CMake variable this spec doesn't own, so its paths pass through (raw ...).
((side-module "imgui_plugin"
	(compiler cxx)
	(flags "--std=c++17" "-fno-exceptions" "-fno-rtti")
	(includes (current) (root "modules/core/include")
		(root "plugins/include") (raw "${imgui_SOURCE_DIR}")
		(raw "${imgui_SOURCE_DIR}/backends"))
	(sources (current "imgui_plugin.cpp") (raw "${imgui_SOURCE_DIR}/imgui.cpp")
		(raw "${imgui_SOURCE_DIR}/imgui_draw.cpp")
		(raw "${imgui_SOURCE_DIR}/imgui_tables.cpp")
		(raw "${imgui_SOURCE_DIR}/imgui_widgets.cpp")
		(raw "${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp"))
	(depends (current "imgui_plugin.cpp") (raw "${imgui_SOURCE_DIR}/imgui.cpp")
		(raw "${imgui_SOURCE_DIR}/imgui_draw.cpp")
		(raw "${imgui_SOURCE_DIR}/imgui_tables.cpp")
		(raw "${imgui_SOURCE_DIR}/imgui_widgets.cpp")
		(raw "${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp"))))
