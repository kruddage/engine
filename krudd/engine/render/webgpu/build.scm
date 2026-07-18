; SPDX-License-Identifier: GPL-2.0-or-later
((library "renderer_webgpu"
	(sources "renderer_webgpu.c" "webgpu_platform.c")
	(private (raw "${generated}") (root "core/include"))
	;;! Natively this needs Dawn's <webgpu/webgpu.h>, which is an out-of-tree
	;;! artifact; without KRUDD_DAWN_PREFIX the whole library is left out of
	;;! the native graph. On WASM the headers come from --use-port=emdawnwebgpu
	;;! and this clause is inert.
	(dawn)
	(link "log" "memory" "subsystem" "subsystem_manager" "script")))
