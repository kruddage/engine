; SPDX-License-Identifier: GPL-2.0-or-later
((library "edit_plugin"
	(sources "edit.c" "edit_plugin.c")
	(public "." (root "modules/include") (root "modules/core/include"))
	(link "subsystem_manager"))
 (native-only
	;; scm-lint:off
	; Link the pure history ops directly so the test needs no plugin glue.
	;; scm-lint:on
	(executable "edit_test"
		(sources "edit_test.c" "edit.c")
		(private "." (root "modules/include"))
		(link "memory"))
	(test "edit" "edit_test")))
