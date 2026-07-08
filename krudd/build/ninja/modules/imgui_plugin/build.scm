; SPDX-License-Identifier: GPL-2.0-or-later
;
; ImGui debug UI shell — a wasm-only module, no native target. imgui itself is
; fetched by krudd (krudd/build/introspect.scm's krudd-fetch) into ${imgui}, a
; path this spec doesn't own, so its paths pass through (raw ...). The C++
; standard and the -fno-exceptions/-fno-rtti that it and third-party imgui build
; with ride on (wasm-flags ...); the emitter puts those on the emcc_cxx sources
; only, so third-party imgui rides in without the project's -Werror/-Wpedantic.
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
