; SPDX-License-Identifier: GPL-2.0-or-later
((wasm-only
	(library "imgui_plugin"
		(wasm-flags "--std=c++17" "-fno-exceptions" "-fno-rtti")
		(sources "imgui_plugin.cpp" (raw "${imgui}/imgui.cpp")
			(raw "${imgui}/imgui_draw.cpp")
			(raw "${imgui}/imgui_tables.cpp")
			(raw "${imgui}/imgui_widgets.cpp")
			(raw "${imgui}/backends/imgui_impl_opengl3.cpp"))
		(private "." (raw "${imgui}") (raw "${imgui}/backends"))
		(link "log" "memory" "subsystem" "subsystem_manager"))))
