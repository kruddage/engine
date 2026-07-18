; SPDX-License-Identifier: GPL-2.0-or-later
((native-only
	(executable "scene_renderer_test"
		(sources "scene_renderer_test.c" "scene_renderer.c"
			(root "render/particles/particles.c")
			(root "math/math.c")
			(root "math/camera.c")
			(raw "${generated}/math_gen.c"))
		(private "." (raw "${generated}")
			(root "render/null")
			(root "render/frame_graph") (root "asset")
			(root "render/particles")
			(root "core/include")
			(root "abi") (raw "../third_party"))
		(link "mesh_script" "texture_script" "frame_graph" "renderer_null"
			"log" "memory" "subsystem_manager" "script" "m"))
	(test "scene_renderer" "scene_renderer_test"))
 (wasm-only
	(library "scene_renderer"
		(sources "scene_renderer.c"
			(root "math/math.c")
			(root "math/camera.c")
			(raw "${generated}/math_gen.c"))
		(private "." (raw "${generated}") (root "render/frame_graph")
			(root "render/particles")
			(root "core/include") (root "abi") (root "asset")
			(raw "../third_party"))
		(link "mesh_script" "texture_script" "frame_graph" "particles"
			"log" "memory" "subsystem" "subsystem_manager" "script"
			"m"))))
