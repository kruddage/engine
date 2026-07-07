; SPDX-License-Identifier: GPL-2.0-or-later
((library "edit_plugin"
	(sources "edit.c" "edit_plugin.c")
	(public "." (root "plugins/include") (root "modules/core/include"))
	(link "subsystem_manager"))
 (native-only
	; Link the pure history ops directly so the test needs no plugin glue.
	(executable "edit_test"
		(sources "edit_test.c" "edit.c")
		(private "." (root "plugins/include"))
		(link "memory"))
	(test "edit" "edit_test"))
 (side-module "edit_plugin"
	(includes (current) (root "modules/core/include")
		(root "plugins/include"))
	(sources (current "edit.c") (current "edit_plugin.c"))
	(depends (current "edit.c") (current "edit_plugin.c"))))
