; SPDX-License-Identifier: GPL-2.0-or-later
;
; Frostbite-style render graph (GPU lent at execute time).
((library "frame_graph"
	(sources "fg.c")
	(public ".")
	(private (root "plugins/renderer"))
	(link "log" "memory" "subsystem" "subsystem_manager"))
 (native-only
	(executable "fg_test"
		(sources "fg_test.c")
		(private "." (root "plugins/renderer")
			(root "plugins/renderer_null"))
		(link "frame_graph" "renderer_null" "log" "memory"
			"subsystem_manager"))
	(test "fg" "fg_test"))
 (side-module "frame_graph"
	(includes (current) (root "plugins/renderer")
		(root "modules/core/include") (root "plugins/include"))
	(sources (current "fg.c"))
	(depends (current "fg.c"))))
