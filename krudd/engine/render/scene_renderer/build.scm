; SPDX-License-Identifier: GPL-2.0-or-later
((native-only
	(executable "scene_renderer_test"
		(sources "scene_renderer_test.c" "scene_renderer.c"
			(root "math/math.c")
			(root "math/camera.c")
			(raw "${generated}/math_gen.c")
			(root "asset/primitives.c"))
		(private "." (raw "${generated}")
			(root "render/null")
			(root "render/frame_graph") (root "asset")
			(root "abi"))
		(link "frame_graph" "renderer_null" "log" "memory"
			"subsystem_manager" "m"))
	(test "scene_renderer" "scene_renderer_test"))
 (wasm-only
	(library "scene_renderer"
		(sources "scene_renderer.c"
			(root "math/math.c")
			(root "math/camera.c")
			(raw "${generated}/math_gen.c"))
		(private "." (raw "${generated}") (root "render/frame_graph")
			(root "core/include") (root "abi"))
		(link "frame_graph" "log" "memory" "subsystem"
			"subsystem_manager" "m"))))
