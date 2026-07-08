; SPDX-License-Identifier: GPL-2.0-or-later
((native-only
	(executable "scene_renderer_test"
		(sources "scene_renderer_test.c" "scene_renderer.c"
			(root "modules/math/math.c")
			(root "modules/math/camera.c")
			(raw "${generated}/math_gen.c")
			(root "modules/asset/primitives.c"))
		(private "." (root "modules/renderer")
			(root "modules/renderer_null")
			(root "modules/frame_graph") (root "modules/asset")
			(root "modules/include"))
		(link "frame_graph" "renderer_null" "log" "memory"
			"subsystem_manager" "m"))
	(test "scene_renderer" "scene_renderer_test"))
 (wasm-only
	(library "scene_renderer"
		(sources "scene_renderer.c"
			(root "modules/math/math.c")
			(root "modules/math/camera.c")
			(raw "${generated}/math_gen.c"))
		(private "." (root "modules/renderer") (root "modules/frame_graph")
			(root "modules/core/include") (root "modules/include"))
		(link "frame_graph" "log" "memory" "subsystem"
			"subsystem_manager" "m"))))
