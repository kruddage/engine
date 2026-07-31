; SPDX-License-Identifier: GPL-2.0-or-later
((library "frame_graph"
	(sources "fg.c")
	(public ".")
	(private (raw "${generated}"))
	(link "log" "memory" "subsystem" "subsystem_manager"))
 (native-only
	(executable "fg_test"
		(sources "fg_test.c")
		(private "." (raw "${generated}")
			(root "render/null"))
		(link "frame_graph" "renderer_null" "log" "memory"
			"subsystem_manager"))
	(test "fg" "fg_test")))
