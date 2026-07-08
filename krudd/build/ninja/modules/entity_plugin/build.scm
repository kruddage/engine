; SPDX-License-Identifier: GPL-2.0-or-later
;; scm-lint:off
;
; Runtime entity/scene system (the "scene" subsystem).
;; scm-lint:on
((library "entity_plugin"
	(sources "entity.c" "entity_plugin.c" "scene_edit.c")
	(public "." (root "modules/include") (root "modules/core/include"))
	(link "memory" "subsystem_manager"))
 (native-only
	;; scm-lint:off
	; Pure world ops linked directly — no plugin glue in the test.
	;; scm-lint:on
	(executable "entity_test"
		(sources "entity_test.c" "entity.c")
		(private "." (root "modules/include")
			(root "modules/core/include")
			(root "modules/memory/include"))
		(link "memory"))
	(test "entity" "entity_test")
	;; scm-lint:off
	; Snapshot-based undo: world ops + edit history linked directly so the
	; test drives the same command path as the plugin.
	;; scm-lint:on
	(executable "scene_edit_test"
		(sources "scene_edit_test.c" "scene_edit.c" "entity.c"
			(root "modules/edit_plugin/edit.c"))
		(private "." (root "modules/include")
			(root "modules/edit_plugin")
			(root "modules/core/include")
			(root "modules/memory/include"))
		(link "memory"))
	(test "scene_edit" "scene_edit_test")))
