; SPDX-License-Identifier: GPL-2.0-or-later
((library "scene_plugin"
	(sources "scene_plugin.c")
	(public "." (root "modules/include") (root "modules/core/include"))
	(link "memory" "subsystem_manager"))
 (native-only
	(executable "scene_test"
		(sources "scene_test.c")
		(link "scene_plugin" "asset_plugin" "log" "memory"
			"subsystem_manager"))
	(test "scene" "scene_test")))
