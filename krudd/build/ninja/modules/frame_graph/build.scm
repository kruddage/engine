; SPDX-License-Identifier: GPL-2.0-or-later
;; scm-lint:off
;
; Frostbite-style render graph (GPU lent at execute time).
;; scm-lint:on
((library "frame_graph"
	(sources "fg.c")
	(public ".")
	(private (root "modules/renderer"))
	(link "log" "memory" "subsystem" "subsystem_manager"))
 (native-only
	(executable "fg_test"
		(sources "fg_test.c")
		(private "." (root "modules/renderer")
			(root "modules/renderer_null"))
		(link "frame_graph" "renderer_null" "log" "memory"
			"subsystem_manager"))
	(test "fg" "fg_test")))
