; SPDX-License-Identifier: GPL-2.0-or-later
((wasm-only
	(library "kruddgui"
		(wasm-flags "--std=c++17" "-fno-exceptions" "-fno-rtti")
		(sources "kruddgui.cpp" "kgui_batch.c" "kgui_input.c"
			"kgui_text_edit.c" "kgui_font.c")
		(private "." (raw "${generated}") (raw "../third_party"))
		(link "script" "log" "memory" "subsystem"
			"subsystem_manager")))

 (native-only
	;;! kgui_batch links libm for the vector primitives' sinf/cosf/sqrtf.
	(library "kgui_batch"
		(sources "kgui_batch.c")
		(public (current))
		(link "m"))
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

	(library "kgui_text_edit"
		(sources "kgui_text_edit.c")
		(public (current)))
	(executable "kgui_text_edit_test"
		(sources "kgui_text_edit_test.c")
		(link "kgui_text_edit"))
	(test "kgui_text_edit" "kgui_text_edit_test")

	(library "kgui_font"
		(sources "kgui_font.c")
		(public (current)))
	(executable "kgui_font_test"
		(sources "kgui_font_test.c")
		(link "kgui_font" "kgui_batch"))
	(test "kgui_font" "kgui_font_test")

	(executable "kgui_scene_test"
		(sources "kgui_scene_test.c")
		(private (root "core/include") (raw "${generated}")
			(raw "../third_party"))
		(link "script"))
	(test "kgui_scene" "kgui_scene_test")

	(executable "kgui_widgets_test"
		(sources "kgui_widgets_test.c")
		(private (root "core/include") (raw "${generated}")
			(raw "../third_party"))
		(link "script"))
	(test "kgui_widgets" "kgui_widgets_test")

	(executable "kgui_assets_test"
		(sources "kgui_assets_test.c")
		(private (root "core/include") (raw "${generated}")
			(raw "../third_party"))
		(link "script"))
	(test "kgui_assets" "kgui_assets_test")))
