; SPDX-License-Identifier: GPL-2.0-or-later
;
; .scene v1 binary decoder, registered as an asset codec.
((library "scene_plugin"
	(sources "scene_plugin.c")
	(public "." (root "plugins/include") (root "modules/core/include"))
	(link "memory" "subsystem_manager"))
 (native-only
	(executable "scene_test"
		(sources "scene_test.c")
		(link "scene_plugin" "asset_plugin" "log" "memory"
			"subsystem_manager"))
	(test "scene" "scene_test"))
 (side-module "scene_plugin"
	(includes (current) (root "modules/core/include")
		(root "plugins/include"))
	(sources (current "scene_plugin.c"))
	(depends (current "scene_plugin.c"))))
