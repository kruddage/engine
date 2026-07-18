; SPDX-License-Identifier: GPL-2.0-or-later
(;;! The id table behind texture-handle. Split out of renderer_webgpu.c and
 ;;! kept free of WebGPU types so it builds — and is tested — without Dawn:
 ;;! the `(dawn)` library below is skipped natively when no prefix is
 ;;! configured, which is every CI run, so logic left inside it is logic CI
 ;;! never compiles.
 (library "webgpu_texture_registry"
	(sources "texture_registry.c")
	(public "."))
 (library "renderer_webgpu"
	(sources "renderer_webgpu.c" "webgpu_platform.c")
	(private (raw "${generated}") (root "core/include"))
	;;! Natively this needs Dawn's <webgpu/webgpu.h>, which is an out-of-tree
	;;! artifact; without KRUDD_DAWN_PREFIX the whole library is left out of
	;;! the native graph. On WASM the headers come from --use-port=emdawnwebgpu
	;;! and this clause is inert.
	(dawn)
	(link "webgpu_texture_registry" "log" "memory" "subsystem"
		"subsystem_manager" "script"))
 (native-only
	(executable "webgpu_texture_registry_test"
		(sources "texture_registry_test.c")
		(private ".")
		(link "webgpu_texture_registry"))
	(test "webgpu_texture_registry" "webgpu_texture_registry_test")))
