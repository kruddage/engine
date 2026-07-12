; SPDX-License-Identifier: GPL-2.0-or-later
((library "subsystem"
	(sources "subsystem.c")
	(public "include"))

 (library "subsystem_manager"
	(sources "subsystem_manager.c")
	(public "include"))

 (library "script"
	(sources "script.c")
	(public "include")
	(private (raw "../third_party") (raw "${generated}"))
	(link "log" "m"))

 (executable "index"
	(sources "engine.c")
	(private "include" (raw "${generated}")
		(root "abi"))
	(link "subsystem" "subsystem_manager" "log" "memory" "script")
	(wasm-modules "asset_plugin" "edit_plugin" "entity_plugin"
		"renderer_webgl" "frame_graph" "scene_renderer" "imgui_plugin"
		"kruddboard" "kruddgui"))

 (native-only
	(executable "subsystem_test"
		(sources "subsystem_test.c")
		(link "subsystem"))
	(test "subsystem" "subsystem_test")

	(executable "subsystem_manager_test"
		(sources "subsystem_manager_test.c")
		(link "subsystem_manager"))
	(test "subsystem_manager" "subsystem_manager_test")

	(executable "script_test"
		(sources "script_test.c")
		(link "script"))
	(test "script" "script_test")

	(executable "shader_transpile_test"
		(sources "shader_transpile_test.c")
		(link "script"))
	(test "shader_transpile" "shader_transpile_test")))
