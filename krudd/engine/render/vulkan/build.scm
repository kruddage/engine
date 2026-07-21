; SPDX-License-Identifier: GPL-2.0-or-later
((library "renderer_vulkan"
   (sources "renderer_vulkan.c" "vulkan_platform.c")
   (public ".")
   (private (raw "${generated}") (root "core/include"))
   ;;! Natively this needs the Vulkan headers + loader (<vulkan/vulkan.h>,
   ;;! -lvulkan), which are a system dependency the editor build installs.
   ;;! Without KRUDD_VULKAN the whole library is left out of the native graph
   ;;! (every ordinary build and every CI run), exactly like the `(dawn)`
   ;;! library — so a plain `krudd build` is byte-for-byte unchanged. It is never
   ;;! in the wasm-modules list, so the web build never compiles it either: the
   ;;! browser keeps WebGL and WebGPU, Vulkan is native only.
   (vulkan)
   (link "log" "subsystem" "subsystem_manager")))
