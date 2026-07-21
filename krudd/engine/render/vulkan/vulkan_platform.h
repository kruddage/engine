/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The platform seam for the native Vulkan backend — the Vulkan analogue of
 * webgpu_platform.h, kept to the same deliberately narrow shape.
 *
 * renderer_vulkan.c owns the instance, device and swapchain, but it must not
 * link a window library (Qt, and its Wayland/X11/Win32 surface entry points):
 * it is shared between a future offscreen harness and the editor, and only the
 * editor has a window. So the one genuinely window-specific step — turning a
 * native window handle into a VkSurfaceKHR — is injected from the outside
 * through this host, exactly as the WebGPU backend takes its WGPUSurface.
 *
 * With a host set (krudd_qt), create_surface returns the window's VkSurfaceKHR
 * and drawable_size reports the window's size; the backend builds a swapchain
 * and presents. With no host set — the default, and the only state a headless
 * build ever sees — create_surface answers VK_NULL_HANDLE and the backend
 * stands up an instance and device but no swapchain, so validation-layer
 * bring-up is still exercised with nothing to present into.
 */
#ifndef KRUDD_VULKAN_PLATFORM_H
#define KRUDD_VULKAN_PLATFORM_H

#include <stdint.h>

#include <vulkan/vulkan.h>

/*
 * Native windowing, injected from the outside. A windowed binary (krudd_qt)
 * owns the window and registers a host here before the backend boots; it is the
 * only thing that knows how to reach the QWindow's native Wayland/X11/Win32
 * handle and which VkCreate*SurfaceKHR to call.
 */
struct vulkan_platform_host {
	/*
	 * Create the presentation surface for the host's window against the
	 * backend's instance. Returns VK_NULL_HANDLE on failure — a real error
	 * here, unlike the no-host case, since a host means a window is expected.
	 */
	VkSurfaceKHR (*create_surface)(VkInstance instance, void *user);
	/* The window's current drawable size, in physical pixels. */
	void         (*drawable_size)(uint32_t *w, uint32_t *h, void *user);
	void         *user;
};

/* Register (or, with NULL, clear) the windowing host. Call before boot. */
void vulkan_platform_set_host(const struct vulkan_platform_host *host);

/*
 * The window's VkSurfaceKHR, or VK_NULL_HANDLE when nothing is hosting us. A
 * NULL return is a supported answer, not a failure: a headless build has no
 * window and the backend configures itself with no swapchain instead.
 */
VkSurfaceKHR vulkan_platform_create_surface(VkInstance instance);

/*
 * Drawable size in physical (device) pixels, never zero. Delegates to the host
 * when one is set; otherwise a fixed fallback so a swapchain-less build still
 * has a sane number to log.
 */
void vulkan_platform_drawable_size(uint32_t *w, uint32_t *h);

#endif /* KRUDD_VULKAN_PLATFORM_H */
