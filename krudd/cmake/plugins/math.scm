; SPDX-License-Identifier: GPL-2.0-or-later
;
; math has no library — the math sources compile straight into the native test
; (and, on WASM, into scene_renderer's side module).
((native-only
	(executable "math_test"
		(sources "math_test.c" "math.c" "camera.c")
		(private (root "plugins/include"))
		(link "m"))
	(test "math" "math_test")))
