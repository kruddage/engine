; SPDX-License-Identifier: GPL-2.0-or-later
;
; WebGL renderer (WASM only) — no native test target.
((library "renderer_webgl"
	(sources "renderer_webgl.c")
	(private (root "plugins/renderer"))
	(link "log" "memory" "subsystem" "subsystem_manager"))
 (side-module "renderer_webgl"
	(includes (current) (root "plugins/renderer")
		(root "modules/core/include") (root "plugins/include"))
	(sources (current "renderer_webgl.c"))
	(depends (current "renderer_webgl.c"))))
