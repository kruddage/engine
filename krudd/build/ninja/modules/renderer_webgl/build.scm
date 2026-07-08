; SPDX-License-Identifier: GPL-2.0-or-later
;
; WebGL renderer. No native test target — the library archives natively (its
; #else branch stubs the GL seam so it compiles), but the real renderer only
; runs in the WASM module, which links this archive like any other module.
((library "renderer_webgl"
	(sources "renderer_webgl.c")
	(private (root "modules/renderer"))
	(link "log" "memory" "subsystem" "subsystem_manager")))
