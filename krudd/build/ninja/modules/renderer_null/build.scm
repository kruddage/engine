; SPDX-License-Identifier: GPL-2.0-or-later
((library "renderer_null"
	(sources "renderer_null.c")
	(private (root "modules/renderer"))
	(link "log" "subsystem" "subsystem_manager"))
 (native-only
	(executable "renderer_null_test"
		(sources "renderer_null_test.c")
		(private "." (root "modules/renderer"))
		(link "renderer_null" "log" "subsystem_manager"))
	(test "renderer_null" "renderer_null_test")))
