; SPDX-License-Identifier: GPL-2.0-or-later
((library "entity_plugin"
	(sources "entity.c" "entity_plugin.c" "entity_script.c" "scene_edit.c")
	(public "." (root "abi") (root "core/include"))
	(private (raw "../third_party"))
	(link "memory" "subsystem_manager" "script"))
 (native-only
	(executable "entity_test"
		(sources "entity_test.c" "entity.c")
		(private "." (root "abi")
			(root "core/include")
			(root "memory/include"))
		(link "memory"))
	(test "entity" "entity_test")
	(executable "scene_edit_test"
		(sources "scene_edit_test.c" "scene_edit.c" "entity.c"
			(root "edit/edit.c"))
		(private "." (root "abi")
			(root "edit")
			(root "core/include")
			(root "memory/include"))
		(link "memory"))
	(test "scene_edit" "scene_edit_test")
	(executable "entity_script_test"
		(sources "entity_script_test.c" "entity_script.c" "entity.c")
		(private "." (root "abi")
			(root "core/include")
			(root "memory/include")
			(raw "../third_party"))
		(link "script" "memory"))
	(test "entity_script" "entity_script_test")))
