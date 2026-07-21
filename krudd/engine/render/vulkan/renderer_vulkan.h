/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The native Vulkan backend's public entry points — the small surface the Qt
 * editor host (krudd_qt.cpp) drives, mirroring renderer_webgpu.h's shape so the
 * host swaps one backend for the other without learning a new boot protocol.
 *
 * Native only: Vulkan is the desktop editor's GPU path (SteamOS / the Deck /
 * Windows). The browser build keeps its WebGL and WebGPU backends and never
 * links this — there is no __EMSCRIPTEN__ half here at all.
 */
#ifndef KRUDD_RENDERER_VULKAN_H
#define KRUDD_RENDERER_VULKAN_H

struct subsystem_manager;

/*
 * Register the Vulkan backend as the "renderer" subsystem. Like the WebGPU
 * backend this resolves "log" from the manager, so it must be registered after
 * log. Device bring-up starts in the subsystem's init and the rest of the boot
 * waits on renderer_vulkan_device_ready().
 */
void renderer_vulkan_plugin_entry(struct subsystem_manager *mgr);

/*
 * Whether the instance, device and (when a window hosts us) swapchain are up and
 * the gpu_api vtable is usable. The host gates the render-cluster boot on this:
 * plugins create GPU resources in their init, none of which can run before there
 * is a device to create them on. Returns 1 once ready, 0 until then (or if
 * bring-up failed).
 */
int renderer_vulkan_device_ready(void);

#endif /* KRUDD_RENDERER_VULKAN_H */
