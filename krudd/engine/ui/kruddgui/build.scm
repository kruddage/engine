; SPDX-License-Identifier: GPL-2.0-or-later
((wasm-only
	(library "kruddgui"
		(wasm-flags "--std=c++17" "-fno-exceptions" "-fno-rtti")
		(sources "kruddgui.cpp" "kgui_batch.c" "kgui_input.c"
			"kgui_font.c")
		(private "." (raw "${generated}") (raw "../third_party")
			(raw "${imgui}") (raw "${imgui}/backends"))
		(link "script" "imgui_plugin" "log" "memory" "subsystem"
			"subsystem_manager")))

 (native-only
	(library "kgui_batch"
		(sources "kgui_batch.c")
		(public (current)))
	(executable "kgui_batch_test"
		(sources "kgui_batch_test.c")
		(link "kgui_batch"))
	(test "kgui_batch" "kgui_batch_test")

	(library "kgui_input"
		(sources "kgui_input.c")
		(public (current)))
	(executable "kgui_input_test"
		(sources "kgui_input_test.c")
		(link "kgui_input"))
	(test "kgui_input" "kgui_input_test")

	;;! The glyph baker is GL-free, so its bake + metrics + kerning are
	;;! host-tested. stb_truetype pulls in libm; kgui_font_test bakes a font
	;;! passed at runtime (KGUI_TEST_FONT / argv) and SKIPs green without one.
	(library "kgui_font"
		(sources "kgui_font.c")
		(public (current))
		(private (raw "../third_party"))
		(link "kgui_batch" "m"))
	(executable "kgui_font_test"
		(sources "kgui_font_test.c")
		(link "kgui_font" "kgui_batch"))
	(test "kgui_font" "kgui_font_test")))
