; SPDX-License-Identifier: GPL-2.0-or-later
((wasm-only
	(library "kruddgui"
		(wasm-flags "--std=c++17" "-fno-exceptions" "-fno-rtti")
		(sources "kruddgui.cpp" "kgui_batch.c" "kgui_input.c")
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
	(test "kgui_input" "kgui_input_test")))
