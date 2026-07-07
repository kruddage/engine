; SPDX-License-Identifier: GPL-2.0-or-later
;
; Headless null renderer, used in native tests.
((library "renderer_null"
	(sources "renderer_null.c")
	(private (root "plugins/renderer"))
	(link "log" "subsystem" "subsystem_manager"))
 (native-only
	(executable "renderer_null_test"
		(sources "renderer_null_test.c")
		(private "." (root "plugins/renderer"))
		(link "renderer_null" "log" "subsystem_manager"))
	(test "renderer_null" "renderer_null_test"))
 (side-module "renderer_null"
	(includes (current) (root "plugins/renderer")
		(root "modules/core/include") (root "plugins/include"))
	(sources (current "renderer_null.c"))
	(depends (current "renderer_null.c"))))
