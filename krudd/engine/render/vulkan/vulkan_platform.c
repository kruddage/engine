/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The native half of the Vulkan platform seam (see vulkan_platform.h). Native
 * only — there is no Emscripten Vulkan target — so unlike webgpu_platform.c this
 * file has no #ifdef, just the host registration and its two delegating shims.
 */
#include "vulkan_platform.h"

/*
 * The windowing host, or NULL. NULL is the default and the only state a
 * headless build ever sees; a windowed binary sets it before boot. Every branch
 * that changes behaviour keys off this one pointer.
 */
static const struct vulkan_platform_host *g_host;

void vulkan_platform_set_host(const struct vulkan_platform_host *host)
{
	g_host = host;
}

VkSurfaceKHR vulkan_platform_create_surface(VkInstance instance)
{
	if (g_host && g_host->create_surface)
		return g_host->create_surface(instance, g_host->user);
	return VK_NULL_HANDLE;
}

void vulkan_platform_drawable_size(uint32_t *w, uint32_t *h)
{
	if (g_host && g_host->drawable_size) {
		g_host->drawable_size(w, h, g_host->user);
		if (*w < 1u)
			*w = 1u;
		if (*h < 1u)
			*h = 1u;
		return;
	}
	*w = 800u;
	*h = 600u;
}
