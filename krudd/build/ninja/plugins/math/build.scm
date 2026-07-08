; SPDX-License-Identifier: GPL-2.0-or-later
;
; math has no library — the math sources compile straight into the native test
; (and, on WASM, into scene_renderer's side module).
;
; mat4_perspective is no longer hand-written here: krudd's monolang emitter
; lowers it from krudd/build/modules/math.scm to ${generated}/math_gen.c, which
; compiles into math_test beside the still-hand-written math.c and camera.c. The
; .scm is the only source of that function in git; math_gen.c is a build output.
((native-only
	(executable "math_test"
		(sources "math_test.c" "math.c" "camera.c"
			 (raw "${generated}/math_gen.c"))
		(private (root "plugins/include"))
		(link "m"))
	(test "math" "math_test")))
