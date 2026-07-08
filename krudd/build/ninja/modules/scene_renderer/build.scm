; SPDX-License-Identifier: GPL-2.0-or-later
;; scm-lint:off
;
; Draws COMPONENT_RENDER entities via the frame graph (#172). No native library
; — the renderer sources compile straight into the native test; the WASM build
; folds them in through a wasm-only library the main module links.
;
; camera.c calls mat4_perspective, now generated from the monolang into
; ${generated}/math_gen.c, so both consumers of the math sources compile that
; generated file too (math has no library to fold it into).
;; scm-lint:on
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
