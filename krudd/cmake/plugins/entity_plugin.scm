; SPDX-License-Identifier: GPL-2.0-or-later
;
; Runtime entity/scene system (the "scene" subsystem).
((library "entity_plugin"
	(sources "entity.c" "entity_plugin.c" "scene_edit.c")
	(public "." (root "plugins/include") (root "modules/core/include"))
	(link "memory" "subsystem_manager"))
 (native-only
	; Pure world ops linked directly — no plugin glue in the test.
	(executable "entity_test"
		(sources "entity_test.c" "entity.c")
		(private "." (root "plugins/include")
			(root "modules/core/include")
			(root "modules/memory/include"))
		(link "memory"))
	(test "entity" "entity_test")
	; Snapshot-based undo: world ops + edit history linked directly so the
	; test drives the same command path as the plugin.
	(executable "scene_edit_test"
		(sources "scene_edit_test.c" "scene_edit.c" "entity.c"
			(root "plugins/edit_plugin/edit.c"))
		(private "." (root "plugins/include")
			(root "plugins/edit_plugin")
			(root "modules/core/include")
			(root "modules/memory/include"))
		(link "memory"))
	(test "scene_edit" "scene_edit_test"))
 (side-module "entity_plugin"
	(includes (current) (root "modules/core/include")
		(root "plugins/include"))
	(sources (current "entity.c") (current "entity_plugin.c")
		(current "scene_edit.c"))
	(depends (current "entity.c") (current "entity_plugin.c")
		(current "scene_edit.c"))))
