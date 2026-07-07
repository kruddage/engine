; SPDX-License-Identifier: GPL-2.0-or-later
;
; Draws COMPONENT_RENDER entities via the frame graph (#172). No library — the
; renderer sources compile into the native test and the WASM side module.
((native-only
	(executable "scene_renderer_test"
		(sources "scene_renderer_test.c" "scene_renderer.c"
			(root "plugins/math/math.c")
			(root "plugins/math/camera.c")
			(root "plugins/asset/primitives.c"))
		(private "." (root "plugins/renderer")
			(root "plugins/renderer_null")
			(root "plugins/frame_graph") (root "plugins/asset")
			(root "plugins/include"))
		(link "frame_graph" "renderer_null" "log" "memory"
			"subsystem_manager" "m"))
	(test "scene_renderer" "scene_renderer_test"))
 (side-module "scene_renderer"
	(includes (current) (root "plugins/renderer")
		(root "plugins/frame_graph")
		(root "modules/core/include") (root "plugins/include"))
	(sources (current "scene_renderer.c")
		(root "plugins/math/math.c")
		(root "plugins/math/camera.c"))
	(depends (current "scene_renderer.c")
		(root "plugins/math/math.c")
		(root "plugins/math/camera.c")
		(root "plugins/include/mesh.h"))))
