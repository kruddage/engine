/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * renderer_vulkan — the native desktop GPU backend, on modern Vulkan.
 *
 * This is the fourth gpu_api provider (beside webgl, webgpu and null): the one
 * the native editor (krudd_qt) runs on SteamOS / the Steam Deck / Windows. The
 * browser build is untouched — it keeps WebGL and WebGPU (via emdawnwebgpu) and
 * never links a line of this. Vulkan is native-only.
 *
 * SCOPE (deliberate, see issue #705). This stands up a *modern, validated*
 * Vulkan base and presents into the window, but it does not yet translate the
 * engine's draw stream:
 *
 *   - REAL: a Vulkan 1.3 instance with the Khronos validation layer and a
 *     VK_EXT_debug_utils messenger wired straight into the engine log, a
 *     physical/logical device and queue, a surface + swapchain from the window
 *     seam, and a genuine per-frame acquire -> clear -> present through
 *     command buffers, semaphores and fences. Everything a validation layer has
 *     an opinion about actually runs, so when this is launched on real hardware
 *     the layer's diagnostics are live and point at real calls.
 *
 *   - STUBBED: the gpu_api draw path (pipeline_create, the cmd_* recording
 *     verbs, buffer/texture creation) are honest no-ops that hand back opaque
 *     placeholder handles. The scene renderer records into them harmlessly and
 *     the viewport shows an animated clear rather than the demo scene. Turning
 *     that into a real forward pass — GLSL/krudd-DSL shaders lowered to SPIR-V,
 *     PSOs, vertex/index/uniform buffers, textures, draws — is the follow-up
 *     the issue explicitly scopes out of this pass.
 *
 * The point of landing it in this shape is the validation base: get the whole
 * engine onto a current Vulkan device with the layers on, so the real renderer
 * is built and debugged against a loader that already talks back.
 */
#include "renderer_vulkan.h"

#include "renderer.h"
#include "vulkan_platform.h"

#include "log.h"
#include "log_api.h"
#include "subsystem.h"
#include "subsystem_manager.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <vulkan/vulkan.h>

/* Native-only backend: the log service is the main module's log_write, exactly
 * as the null renderer wires it for its native harness. */
static const struct log_api native_log = { .write = log_write };
static const struct log_api *g_log = &native_log;

/* How deep the acquire/submit pipeline runs. Two frames in flight keeps the GPU
 * fed without unbounded latency — the textbook default. */
#define VK_FRAMES_IN_FLIGHT 2
/* A swapchain almost never has more than a handful of images; cap the fixed
 * arrays generously and clamp to it, rather than heap-allocate for a scaffold. */
#define VK_MAX_SWAP_IMAGES  8

/* ----------------------------------------------------------------- state --- */

static VkInstance               g_instance;
static VkDebugUtilsMessengerEXT g_debug_messenger;
static VkSurfaceKHR             g_surface;
static VkPhysicalDevice         g_phys;
static uint32_t                 g_queue_family;
static VkDevice                 g_device;
static VkQueue                  g_queue;

static VkSwapchainKHR g_swapchain;
static VkFormat       g_swap_format;
static VkExtent2D     g_swap_extent;
static uint32_t       g_image_count;
static VkImage        g_images[VK_MAX_SWAP_IMAGES];
static VkImageView    g_views[VK_MAX_SWAP_IMAGES];

static VkCommandPool   g_cmd_pool;
static VkCommandBuffer g_cmd_bufs[VK_FRAMES_IN_FLIGHT];
static VkSemaphore     g_acquire_sems[VK_FRAMES_IN_FLIGHT];
static VkFence         g_frame_fences[VK_FRAMES_IN_FLIGHT];
/* One "render finished" semaphore per swapchain image: a present waits on it,
 * so it must not be reused until that present is done, which is naturally
 * per-image rather than per-frame-in-flight. */
static VkSemaphore     g_present_sems[VK_MAX_SWAP_IMAGES];

static uint32_t g_frame;        /* frame-in-flight cursor, 0..VK_FRAMES_IN_FLIGHT-1 */
static uint64_t g_tick;         /* frames presented, for the animated clear */
static int      g_ready;        /* instance + device up (vtable usable) */
static int      g_have_present; /* a surface + swapchain exist to present into */

/* ------------------------------------------------------- validation glue --- */

static VKAPI_ATTR VkBool32 VKAPI_CALL
vk_debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
	    VkDebugUtilsMessageTypeFlagsEXT types,
	    const VkDebugUtilsMessengerCallbackDataEXT *data,
	    void *user)
{
	enum log_level level = LOG_LEVEL_INFO;

	(void)types;
	(void)user;
	if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
		level = LOG_LEVEL_ERROR;
	else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
		level = LOG_LEVEL_WARN;
	g_log->write(level, "vulkan(validation): %s",
		     (data && data->pMessage) ? data->pMessage : "(no message)");
	/* VK_FALSE: never abort the offending call — this is diagnostics, not a
	 * gate. The message is already logged above. */
	return VK_FALSE;
}

static void debug_messenger_ci(VkDebugUtilsMessengerCreateInfoEXT *ci)
{
	memset(ci, 0, sizeof(*ci));
	ci->sType =
		VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
	ci->messageSeverity =
		VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
		VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
		VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
		VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
	ci->messageType =
		VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
		VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
		VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
	ci->pfnUserCallback = vk_debug_cb;
}

/* Is `name` present in the enumerated extension list? */
static int has_ext(const VkExtensionProperties *exts, uint32_t count,
		   const char *name)
{
	uint32_t i;

	for (i = 0; i < count; i++)
		if (strcmp(exts[i].extensionName, name) == 0)
			return 1;
	return 0;
}

/* Is `name` already in the list being assembled? has_ext answers the same
 * question against the loader's VkExtensionProperties; this one works on the
 * plain string list, so a host that also names VK_KHR_surface cannot get it
 * enabled twice. */
static int has_name(const char *const *list, uint32_t count, const char *name)
{
	uint32_t i;

	for (i = 0; i < count; i++)
		if (strcmp(list[i], name) == 0)
			return 1;
	return 0;
}

/* Is the Khronos validation layer installed? Enabling it blindly fails instance
 * creation on a machine without the validation-layer package, so probe first. */
static int validation_available(void)
{
	VkLayerProperties props[64];
	uint32_t count = 0;
	uint32_t i;

	if (vkEnumerateInstanceLayerProperties(&count, NULL) != VK_SUCCESS)
		return 0;
	if (count > 64)
		count = 64;
	if (vkEnumerateInstanceLayerProperties(&count, props) != VK_SUCCESS)
		return 0;
	for (i = 0; i < count; i++)
		if (strcmp(props[i].layerName, "VK_LAYER_KHRONOS_validation")
		    == 0)
			return 1;
	return 0;
}

/* ------------------------------------------------------- instance / device */

static const char *k_validation_layer = "VK_LAYER_KHRONOS_validation";

static int create_instance(void)
{
	/* What the backend needs on its own account, with no window in play: the
	 * base surface extension and the debug-utils extension the messenger
	 * needs. The per-windowing-system extensions are NOT listed here — this
	 * file must not know whether we are on Wayland, X11 or Win32. They come
	 * from the platform host below, which is the one thing that does know. */
	static const char *const wanted[] = {
		VK_KHR_SURFACE_EXTENSION_NAME,
		VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
	};
	VkExtensionProperties avail[256];
	const char *enabled[16];
	const char *const *host_exts = NULL;
	uint32_t host_count;
	uint32_t host_enabled = 0;
	uint32_t enabled_count = 0;
	uint32_t avail_count = 0;
	int want_validation;
	uint32_t i;
	VkApplicationInfo app;
	VkInstanceCreateInfo ci;
	VkDebugUtilsMessengerCreateInfoEXT dbg;
	VkResult r;

	vkEnumerateInstanceExtensionProperties(NULL, &avail_count, NULL);
	if (avail_count > 256)
		avail_count = 256;
	vkEnumerateInstanceExtensionProperties(NULL, &avail_count, avail);

	for (i = 0; i < sizeof(wanted) / sizeof(wanted[0]); i++) {
		if (has_ext(avail, avail_count, wanted[i]) &&
		    enabled_count < sizeof(enabled) / sizeof(enabled[0]))
			enabled[enabled_count++] = wanted[i];
	}

	/* Then whatever the window host says its VkCreate*SurfaceKHR call needs.
	 * Nothing when no host is registered — the headless case, which wants no
	 * surface extension at all. A host may list more than the box actually
	 * has (both Wayland and XCB on Linux is normal), so the same availability
	 * filter applies: an extension the loader does not report is dropped
	 * here rather than taking instance creation down, and the surface call
	 * that needed it reports its own failure later. */
	host_count = vulkan_platform_instance_extensions(&host_exts);
	for (i = 0; i < host_count; i++) {
		if (has_ext(avail, avail_count, host_exts[i]) &&
		    !has_name(enabled, enabled_count, host_exts[i]) &&
		    enabled_count < sizeof(enabled) / sizeof(enabled[0])) {
			enabled[enabled_count++] = host_exts[i];
			host_enabled++;
		}
	}

	memset(&app, 0, sizeof(app));
	app.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	app.pApplicationName   = "krudd";
	app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
	app.pEngineName        = "krudd";
	app.engineVersion      = VK_MAKE_VERSION(0, 1, 0);
	/* Modern Vulkan: ask for 1.3. The loader clamps to what it supports and
	 * reports the real device version later; requesting 1.3 is what lets the
	 * backend use 1.3 features (dynamic rendering) below. */
	app.apiVersion         = VK_API_VERSION_1_3;

	want_validation = validation_available();
	if (!want_validation)
		g_log->write(LOG_LEVEL_WARN,
			     "renderer_vulkan: %s not installed — running "
			     "without validation layers (install the Vulkan "
			     "validation layers to get diagnostics)",
			     k_validation_layer);

	debug_messenger_ci(&dbg);

	memset(&ci, 0, sizeof(ci));
	ci.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	ci.pApplicationInfo        = &app;
	ci.enabledExtensionCount   = enabled_count;
	ci.ppEnabledExtensionNames = enabled;
	if (want_validation) {
		ci.enabledLayerCount   = 1;
		ci.ppEnabledLayerNames = &k_validation_layer;
		/* Chain the messenger onto instance creation so validation
		 * messages from vkCreateInstance/vkDestroyInstance themselves
		 * are reported, not just those between create and destroy. */
		ci.pNext               = &dbg;
	}

	r = vkCreateInstance(&ci, NULL, &g_instance);
	if (r != VK_SUCCESS) {
		g_log->write(LOG_LEVEL_ERROR,
			     "renderer_vulkan: vkCreateInstance failed (%d)",
			     (int)r);
		return 0;
	}

	/* The persistent messenger (the pNext one only covers instance
	 * creation/destruction). Extension entry points are not exported
	 * symbols; load them through the instance. */
	if (want_validation) {
		PFN_vkCreateDebugUtilsMessengerEXT create =
			(PFN_vkCreateDebugUtilsMessengerEXT)
			vkGetInstanceProcAddr(g_instance,
					      "vkCreateDebugUtilsMessengerEXT");
		if (create)
			create(g_instance, &dbg, NULL, &g_debug_messenger);
	}

	/* The extension count is the tell when the editor will not present: the
	 * window host's extensions are what make VkCreate*SurfaceKHR legal, so
	 * "0 from window host" with a host registered means a surface failure is
	 * already guaranteed, several log lines before it happens. */
	g_log->write(LOG_LEVEL_INFO,
		     "renderer_vulkan: instance up (Vulkan 1.3 requested, %u "
		     "extensions [%u from window host], validation %s)",
		     enabled_count, host_enabled,
		     want_validation ? "on" : "off");
	return 1;
}

static void destroy_debug_messenger(void)
{
	PFN_vkDestroyDebugUtilsMessengerEXT destroy;

	if (!g_instance || !g_debug_messenger)
		return;
	destroy = (PFN_vkDestroyDebugUtilsMessengerEXT)
		vkGetInstanceProcAddr(g_instance,
				      "vkDestroyDebugUtilsMessengerEXT");
	if (destroy)
		destroy(g_instance, g_debug_messenger, NULL);
	g_debug_messenger = VK_NULL_HANDLE;
}

/*
 * Find a queue family that can do graphics — and, when we have a surface, also
 * present to it. A single family that does both is the overwhelmingly common
 * case (every desktop GPU), and taking only that case keeps the swapchain glue
 * to one queue; a split-queue device would need a second queue here, which a
 * scaffold does not.
 */
static int pick_queue_family(VkPhysicalDevice dev, uint32_t *out_family)
{
	VkQueueFamilyProperties props[32];
	uint32_t count = 0;
	uint32_t i;

	vkGetPhysicalDeviceQueueFamilyProperties(dev, &count, NULL);
	if (count > 32)
		count = 32;
	vkGetPhysicalDeviceQueueFamilyProperties(dev, &count, props);

	for (i = 0; i < count; i++) {
		VkBool32 present = VK_TRUE;

		if (!(props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
			continue;
		if (g_surface)
			vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, g_surface,
							     &present);
		if (present) {
			*out_family = i;
			return 1;
		}
	}
	return 0;
}

static int pick_physical_device(void)
{
	VkPhysicalDevice devs[16];
	uint32_t count = 0;
	uint32_t i;
	int best = -1;
	uint32_t best_family = 0;
	int best_discrete = 0;

	vkEnumeratePhysicalDevices(g_instance, &count, NULL);
	if (count == 0) {
		g_log->write(LOG_LEVEL_ERROR,
			     "renderer_vulkan: no Vulkan physical devices");
		return 0;
	}
	if (count > 16)
		count = 16;
	vkEnumeratePhysicalDevices(g_instance, &count, devs);

	/* Prefer a discrete GPU, but take any device with a usable queue. */
	for (i = 0; i < count; i++) {
		VkPhysicalDeviceProperties props;
		uint32_t family;

		if (!pick_queue_family(devs[i], &family))
			continue;
		vkGetPhysicalDeviceProperties(devs[i], &props);
		if (best < 0 ||
		    (!best_discrete &&
		     props.deviceType ==
		     VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)) {
			best         = (int)i;
			best_family  = family;
			best_discrete =
				props.deviceType ==
				VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
		}
	}
	if (best < 0) {
		g_log->write(LOG_LEVEL_ERROR,
			     "renderer_vulkan: no device with a graphics%s "
			     "queue", g_surface ? "+present" : "");
		return 0;
	}

	g_phys         = devs[best];
	g_queue_family = best_family;
	{
		VkPhysicalDeviceProperties props;

		vkGetPhysicalDeviceProperties(g_phys, &props);
		g_log->write(LOG_LEVEL_INFO,
			     "renderer_vulkan: device '%s' (Vulkan %u.%u.%u, "
			     "queue family %u)",
			     props.deviceName,
			     VK_VERSION_MAJOR(props.apiVersion),
			     VK_VERSION_MINOR(props.apiVersion),
			     VK_VERSION_PATCH(props.apiVersion),
			     g_queue_family);
	}
	return 1;
}

static int create_device(void)
{
	float priority = 1.0f;
	const char *dev_exts[1];
	uint32_t dev_ext_count = 0;
	VkDeviceQueueCreateInfo q;
	VkPhysicalDeviceVulkan13Features feats13;
	VkPhysicalDeviceFeatures2 feats2;
	VkDeviceCreateInfo ci;
	VkResult r;

	if (g_surface)
		dev_exts[dev_ext_count++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;

	memset(&q, 0, sizeof(q));
	q.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	q.queueFamilyIndex = g_queue_family;
	q.queueCount       = 1;
	q.pQueuePriorities = &priority;

	/* Dynamic rendering (core in 1.3) is the modern present path used below —
	 * no VkRenderPass / VkFramebuffer objects. Request it through the 1.3
	 * feature struct chained onto features2. */
	memset(&feats13, 0, sizeof(feats13));
	feats13.sType =
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
	feats13.dynamicRendering = VK_TRUE;

	memset(&feats2, 0, sizeof(feats2));
	feats2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
	feats2.pNext = &feats13;

	memset(&ci, 0, sizeof(ci));
	ci.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	ci.pNext                   = &feats2;
	ci.queueCreateInfoCount    = 1;
	ci.pQueueCreateInfos       = &q;
	ci.enabledExtensionCount   = dev_ext_count;
	ci.ppEnabledExtensionNames = dev_ext_count ? dev_exts : NULL;

	r = vkCreateDevice(g_phys, &ci, NULL, &g_device);
	if (r != VK_SUCCESS) {
		g_log->write(LOG_LEVEL_ERROR,
			     "renderer_vulkan: vkCreateDevice failed (%d)",
			     (int)r);
		return 0;
	}
	vkGetDeviceQueue(g_device, g_queue_family, 0, &g_queue);
	return 1;
}

/* ------------------------------------------------------------- swapchain --- */

static VkSurfaceFormatKHR choose_surface_format(void)
{
	VkSurfaceFormatKHR formats[64];
	uint32_t count = 0;
	uint32_t i;
	VkSurfaceFormatKHR chosen;

	vkGetPhysicalDeviceSurfaceFormatsKHR(g_phys, g_surface, &count, NULL);
	if (count > 64)
		count = 64;
	vkGetPhysicalDeviceSurfaceFormatsKHR(g_phys, g_surface, &count,
					     formats);
	/* BGRA8 sRGB is the near-universal swapchain format; fall back to
	 * whatever the surface offers first if it is somehow absent. */
	chosen = formats[0];
	for (i = 0; i < count; i++) {
		if (formats[i].format == VK_FORMAT_B8G8R8A8_SRGB &&
		    formats[i].colorSpace ==
		    VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
			chosen = formats[i];
			break;
		}
	}
	return chosen;
}

static void destroy_swapchain(void)
{
	uint32_t i;

	for (i = 0; i < g_image_count; i++) {
		if (g_views[i])
			vkDestroyImageView(g_device, g_views[i], NULL);
		g_views[i]  = VK_NULL_HANDLE;
		g_images[i] = VK_NULL_HANDLE;
	}
	g_image_count = 0;
	if (g_swapchain) {
		vkDestroySwapchainKHR(g_device, g_swapchain, NULL);
		g_swapchain = VK_NULL_HANDLE;
	}
}

static int create_swapchain(void)
{
	VkSurfaceCapabilitiesKHR caps;
	VkSurfaceFormatKHR format;
	VkExtent2D extent;
	uint32_t desired;
	uint32_t i;
	VkSwapchainCreateInfoKHR ci;
	VkResult r;

	if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_phys, g_surface, &caps)
	    != VK_SUCCESS)
		return 0;

	format = choose_surface_format();
	g_swap_format = format.format;

	if (caps.currentExtent.width != 0xffffffffu) {
		extent = caps.currentExtent;
	} else {
		uint32_t w, h;

		vulkan_platform_drawable_size(&w, &h);
		extent.width  = w;
		extent.height = h;
		if (extent.width < caps.minImageExtent.width)
			extent.width = caps.minImageExtent.width;
		if (extent.width > caps.maxImageExtent.width)
			extent.width = caps.maxImageExtent.width;
		if (extent.height < caps.minImageExtent.height)
			extent.height = caps.minImageExtent.height;
		if (extent.height > caps.maxImageExtent.height)
			extent.height = caps.maxImageExtent.height;
	}
	/* A minimised window reports 0x0; there is nothing to present into. */
	if (extent.width == 0 || extent.height == 0)
		return 0;
	g_swap_extent = extent;

	desired = caps.minImageCount + 1;
	if (caps.maxImageCount > 0 && desired > caps.maxImageCount)
		desired = caps.maxImageCount;
	if (desired > VK_MAX_SWAP_IMAGES)
		desired = VK_MAX_SWAP_IMAGES;

	memset(&ci, 0, sizeof(ci));
	ci.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	ci.surface          = g_surface;
	ci.minImageCount    = desired;
	ci.imageFormat      = format.format;
	ci.imageColorSpace  = format.colorSpace;
	ci.imageExtent      = extent;
	ci.imageArrayLayers = 1;
	ci.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	ci.preTransform     = caps.currentTransform;
	ci.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	/* FIFO is the always-supported, tear-free present mode — the right
	 * default for an editor viewport. */
	ci.presentMode      = VK_PRESENT_MODE_FIFO_KHR;
	ci.clipped          = VK_TRUE;

	r = vkCreateSwapchainKHR(g_device, &ci, NULL, &g_swapchain);
	if (r != VK_SUCCESS) {
		g_log->write(LOG_LEVEL_ERROR,
			     "renderer_vulkan: vkCreateSwapchainKHR failed (%d)",
			     (int)r);
		return 0;
	}

	g_image_count = 0;
	vkGetSwapchainImagesKHR(g_device, g_swapchain, &g_image_count, NULL);
	if (g_image_count > VK_MAX_SWAP_IMAGES)
		g_image_count = VK_MAX_SWAP_IMAGES;
	vkGetSwapchainImagesKHR(g_device, g_swapchain, &g_image_count,
				g_images);

	for (i = 0; i < g_image_count; i++) {
		VkImageViewCreateInfo vi;

		memset(&vi, 0, sizeof(vi));
		vi.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		vi.image    = g_images[i];
		vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vi.format   = g_swap_format;
		vi.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
		vi.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
		vi.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
		vi.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
		vi.subresourceRange.aspectMask =
			VK_IMAGE_ASPECT_COLOR_BIT;
		vi.subresourceRange.levelCount = 1;
		vi.subresourceRange.layerCount = 1;
		if (vkCreateImageView(g_device, &vi, NULL, &g_views[i])
		    != VK_SUCCESS) {
			g_log->write(LOG_LEVEL_ERROR,
				     "renderer_vulkan: image view %u failed", i);
			return 0;
		}
	}

	g_log->write(LOG_LEVEL_INFO,
		     "renderer_vulkan: swapchain %ux%u, %u images",
		     g_swap_extent.width, g_swap_extent.height, g_image_count);
	return 1;
}

static int create_frame_resources(void)
{
	VkCommandPoolCreateInfo pci;
	VkCommandBufferAllocateInfo ai;
	VkSemaphoreCreateInfo si;
	VkFenceCreateInfo fi;
	uint32_t i;

	memset(&pci, 0, sizeof(pci));
	pci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pci.queueFamilyIndex = g_queue_family;
	if (vkCreateCommandPool(g_device, &pci, NULL, &g_cmd_pool)
	    != VK_SUCCESS)
		return 0;

	memset(&ai, 0, sizeof(ai));
	ai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	ai.commandPool        = g_cmd_pool;
	ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	ai.commandBufferCount = VK_FRAMES_IN_FLIGHT;
	if (vkAllocateCommandBuffers(g_device, &ai, g_cmd_bufs) != VK_SUCCESS)
		return 0;

	memset(&si, 0, sizeof(si));
	si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	memset(&fi, 0, sizeof(fi));
	fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	/* Created signalled so the very first frame's fence wait returns at once
	 * instead of deadlocking on a fence nothing has submitted yet. */
	fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;

	for (i = 0; i < VK_FRAMES_IN_FLIGHT; i++) {
		if (vkCreateSemaphore(g_device, &si, NULL, &g_acquire_sems[i])
		    != VK_SUCCESS)
			return 0;
		if (vkCreateFence(g_device, &fi, NULL, &g_frame_fences[i])
		    != VK_SUCCESS)
			return 0;
	}
	for (i = 0; i < g_image_count; i++) {
		if (vkCreateSemaphore(g_device, &si, NULL, &g_present_sems[i])
		    != VK_SUCCESS)
			return 0;
	}
	return 1;
}

static int recreate_swapchain(void)
{
	uint32_t i;

	vkDeviceWaitIdle(g_device);
	for (i = 0; i < g_image_count; i++) {
		if (g_present_sems[i])
			vkDestroySemaphore(g_device, g_present_sems[i], NULL);
		g_present_sems[i] = VK_NULL_HANDLE;
	}
	destroy_swapchain();
	if (!create_swapchain())
		return 0;
	{
		VkSemaphoreCreateInfo si;

		memset(&si, 0, sizeof(si));
		si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
		for (i = 0; i < g_image_count; i++)
			if (vkCreateSemaphore(g_device, &si, NULL,
					      &g_present_sems[i]) != VK_SUCCESS)
				return 0;
	}
	return 1;
}

/* -------------------------------------------------------------- present --- */

/* Record one command buffer that transitions the acquired image to a colour
 * attachment, clears it (an animated colour, so the window visibly lives),
 * and transitions it to present. Dynamic rendering — the modern, render-pass-
 * object-free path. */
static void record_clear(VkCommandBuffer cmd, uint32_t image_index)
{
	VkCommandBufferBeginInfo bi;
	VkImageMemoryBarrier to_color;
	VkImageMemoryBarrier to_present;
	VkRenderingAttachmentInfo color;
	VkRenderingInfo ri;
	VkClearValue clear;
	float t = (float)(g_tick % 240u) / 240.0f;

	memset(&bi, 0, sizeof(bi));
	bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd, &bi);

	memset(&to_color, 0, sizeof(to_color));
	to_color.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	to_color.srcAccessMask = 0;
	to_color.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	to_color.oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
	to_color.newLayout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	to_color.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	to_color.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	to_color.image         = g_images[image_index];
	to_color.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	to_color.subresourceRange.levelCount = 1;
	to_color.subresourceRange.layerCount = 1;
	vkCmdPipelineBarrier(cmd,
			     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
			     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			     0, 0, NULL, 0, NULL, 1, &to_color);

	/* A slow blue<->teal pulse: obviously alive, obviously a placeholder. */
	clear.color.float32[0] = 0.05f;
	clear.color.float32[1] = 0.10f + 0.20f * t;
	clear.color.float32[2] = 0.25f + 0.20f * t;
	clear.color.float32[3] = 1.0f;

	memset(&color, 0, sizeof(color));
	color.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
	color.imageView   = g_views[image_index];
	color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	color.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
	color.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
	color.clearValue  = clear;

	memset(&ri, 0, sizeof(ri));
	ri.sType                 = VK_STRUCTURE_TYPE_RENDERING_INFO;
	ri.renderArea.extent     = g_swap_extent;
	ri.layerCount            = 1;
	ri.colorAttachmentCount  = 1;
	ri.pColorAttachments     = &color;
	vkCmdBeginRendering(cmd, &ri);
	/* No draws yet — see the SCOPE note at the top of the file. */
	vkCmdEndRendering(cmd);

	memset(&to_present, 0, sizeof(to_present));
	to_present.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	to_present.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	to_present.dstAccessMask = 0;
	to_present.oldLayout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	to_present.newLayout     = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	to_present.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	to_present.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	to_present.image         = g_images[image_index];
	to_present.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	to_present.subresourceRange.levelCount = 1;
	to_present.subresourceRange.layerCount = 1;
	vkCmdPipelineBarrier(cmd,
			     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			     VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
			     0, 0, NULL, 0, NULL, 1, &to_present);

	vkEndCommandBuffer(cmd);
}

static void present_frame(void)
{
	VkFence fence = g_frame_fences[g_frame];
	VkSemaphore acquire = g_acquire_sems[g_frame];
	VkCommandBuffer cmd = g_cmd_bufs[g_frame];
	uint32_t image_index = 0;
	VkResult r;
	VkPipelineStageFlags wait_stage =
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	VkSubmitInfo submit;
	VkPresentInfoKHR present;
	VkSemaphore signal;

	vkWaitForFences(g_device, 1, &fence, VK_TRUE, UINT64_MAX);

	r = vkAcquireNextImageKHR(g_device, g_swapchain, UINT64_MAX, acquire,
				  VK_NULL_HANDLE, &image_index);
	if (r == VK_ERROR_OUT_OF_DATE_KHR) {
		recreate_swapchain();
		return;
	}
	if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
		return;

	vkResetFences(g_device, 1, &fence);
	vkResetCommandBuffer(cmd, 0);
	record_clear(cmd, image_index);

	signal = g_present_sems[image_index];

	memset(&submit, 0, sizeof(submit));
	submit.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.waitSemaphoreCount   = 1;
	submit.pWaitSemaphores      = &acquire;
	submit.pWaitDstStageMask    = &wait_stage;
	submit.commandBufferCount   = 1;
	submit.pCommandBuffers      = &cmd;
	submit.signalSemaphoreCount = 1;
	submit.pSignalSemaphores    = &signal;
	vkQueueSubmit(g_queue, 1, &submit, fence);

	memset(&present, 0, sizeof(present));
	present.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	present.waitSemaphoreCount = 1;
	present.pWaitSemaphores    = &signal;
	present.swapchainCount     = 1;
	present.pSwapchains        = &g_swapchain;
	present.pImageIndices      = &image_index;
	r = vkQueuePresentKHR(g_queue, &present);
	if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
		recreate_swapchain();

	g_frame = (g_frame + 1u) % VK_FRAMES_IN_FLIGHT;
	g_tick++;
}

/* ----------------------------------------------------- gpu_api: stubbed --- */
/*
 * The draw path is not yet translated to Vulkan (see the SCOPE note). These
 * satisfy the vtable so the scene renderer records into a live "renderer"
 * subsystem without special-casing a missing backend, but they do no GPU work —
 * the frame the window shows is present_frame()'s animated clear. Opaque
 * placeholder handles come from static pools so the "non-NULL means live"
 * contract the callers rely on holds; nothing dereferences them.
 */
static char     g_handle_pool[256];
static uint32_t g_handle_next;

static void *stub_handle(void)
{
	return &g_handle_pool[g_handle_next++ % sizeof(g_handle_pool)];
}

static gpu_cmd_buf_t vk_cmd_buf_begin(void) { return (gpu_cmd_buf_t)stub_handle(); }
static void vk_cmd_buf_submit(gpu_cmd_buf_t cmd) { (void)cmd; }

/* The frame boundary the host drives once per tick — where the real Vulkan work
 * lives. Everything above records nothing; this acquires, clears and presents. */
static void vk_frame_end(void)
{
	if (g_have_present)
		present_frame();
}

static gpu_pipeline_t vk_pipeline_create(const struct gpu_pipeline_desc *desc)
{
	(void)desc;
	return NULL;
}
static void vk_pipeline_destroy(gpu_pipeline_t p) { (void)p; }
static void vk_cmd_set_pipeline(gpu_cmd_buf_t c, gpu_pipeline_t p)
{
	(void)c;
	(void)p;
}

static gpu_buffer_t vk_buffer_create(const struct gpu_buffer_desc *desc)
{
	(void)desc;
	return (gpu_buffer_t)stub_handle();
}
static void vk_buffer_destroy(gpu_buffer_t b) { (void)b; }
static void vk_buffer_update(gpu_buffer_t b, uint32_t off, const void *d,
			     uint32_t sz)
{
	(void)b;
	(void)off;
	(void)d;
	(void)sz;
}
static void vk_cmd_bind_vertex_buffer(gpu_cmd_buf_t c, uint32_t slot,
				      gpu_buffer_t b, uint32_t off)
{
	(void)c;
	(void)slot;
	(void)b;
	(void)off;
}
static void vk_cmd_bind_index_buffer(gpu_cmd_buf_t c, gpu_buffer_t b,
				     uint32_t off, gpu_index_format fmt)
{
	(void)c;
	(void)b;
	(void)off;
	(void)fmt;
}
static void vk_cmd_bind_uniform_buffer(gpu_cmd_buf_t c, uint32_t slot,
				       gpu_buffer_t b, uint32_t off,
				       uint32_t sz)
{
	(void)c;
	(void)slot;
	(void)b;
	(void)off;
	(void)sz;
}

static void vk_cmd_begin_render_pass(gpu_cmd_buf_t c,
				     const struct gpu_render_pass_desc *d)
{
	(void)c;
	(void)d;
}
static void vk_cmd_end_render_pass(gpu_cmd_buf_t c) { (void)c; }
static void vk_cmd_barrier(gpu_cmd_buf_t c, const struct gpu_barrier *b,
			   uint32_t n)
{
	(void)c;
	(void)b;
	(void)n;
}
static void vk_cmd_draw_indexed(gpu_cmd_buf_t c,
				const struct gpu_draw_indexed_args *a)
{
	(void)c;
	(void)a;
}
static void vk_cmd_draw(gpu_cmd_buf_t c, uint32_t vc, uint32_t ic, uint32_t fv,
			uint32_t fi)
{
	(void)c;
	(void)vc;
	(void)ic;
	(void)fv;
	(void)fi;
}
static void vk_cmd_set_scissor(gpu_cmd_buf_t c, int32_t x, int32_t y,
			       uint32_t w, uint32_t h)
{
	(void)c;
	(void)x;
	(void)y;
	(void)w;
	(void)h;
}
static void vk_cmd_dispatch(gpu_cmd_buf_t c, uint32_t x, uint32_t y, uint32_t z)
{
	(void)c;
	(void)x;
	(void)y;
	(void)z;
}

static void *vk_gpu_malloc(size_t size) { (void)size; return NULL; }
static void vk_gpu_free(void *ptr) { (void)ptr; }

static gpu_texture_t vk_texture_create(const struct gpu_texture_desc *desc)
{
	(void)desc;
	return (gpu_texture_t)stub_handle();
}
static void vk_texture_destroy(gpu_texture_t t) { (void)t; }
static void vk_cmd_bind_texture(gpu_cmd_buf_t c, uint32_t unit,
				gpu_texture_t t)
{
	(void)c;
	(void)unit;
	(void)t;
}
static uint32_t vk_texture_handle(gpu_texture_t t) { (void)t; return 0u; }
static void vk_cmd_bind_texture_handle(gpu_cmd_buf_t c, uint32_t unit,
				       uint32_t handle)
{
	(void)c;
	(void)unit;
	(void)handle;
}

static const struct gpu_api vulkan_api = {
	/* Vulkan's honest conventions: [0,1] clip-space depth (like WebGPU/D3D),
	 * indexed and direct draws. MSAA-resolve/compute/bindless are left clear
	 * until the real draw path lands. */
	.caps                    = GPU_CAP_DRAW_DIRECT | GPU_CAP_DRAW_INDEXED |
				   GPU_CAP_CLIP_Z_ZERO_TO_ONE,
	.cmd_buf_begin           = vk_cmd_buf_begin,
	.cmd_buf_submit          = vk_cmd_buf_submit,
	.frame_end               = vk_frame_end,
	.pipeline_create         = vk_pipeline_create,
	.pipeline_destroy        = vk_pipeline_destroy,
	.cmd_set_pipeline        = vk_cmd_set_pipeline,
	.buffer_create           = vk_buffer_create,
	.buffer_destroy          = vk_buffer_destroy,
	.buffer_update           = vk_buffer_update,
	.cmd_bind_vertex_buffer  = vk_cmd_bind_vertex_buffer,
	.cmd_bind_index_buffer   = vk_cmd_bind_index_buffer,
	.cmd_bind_uniform_buffer = vk_cmd_bind_uniform_buffer,
	.cmd_begin_render_pass   = vk_cmd_begin_render_pass,
	.cmd_end_render_pass     = vk_cmd_end_render_pass,
	.cmd_barrier             = vk_cmd_barrier,
	.cmd_draw_indexed        = vk_cmd_draw_indexed,
	.cmd_draw                = vk_cmd_draw,
	.cmd_set_scissor         = vk_cmd_set_scissor,
	.cmd_dispatch            = vk_cmd_dispatch,
	.gpu_malloc              = vk_gpu_malloc,
	.gpu_free                = vk_gpu_free,
	.gpu_host_to_device_ptr  = NULL,
	.texture_create          = vk_texture_create,
	.texture_destroy         = vk_texture_destroy,
	.cmd_bind_texture        = vk_cmd_bind_texture,
	.texture_handle          = vk_texture_handle,
	.cmd_bind_texture_handle = vk_cmd_bind_texture_handle,
};

/* ------------------------------------------------------- subsystem glue --- */

static void renderer_vulkan_init(void)
{
	if (!create_instance())
		return;

	/* Ask the window seam for a surface. VK_NULL_HANDLE is a supported
	 * answer (no window hosting us): the instance and device still come up
	 * so validation bring-up is exercised, there is just nothing to present
	 * into and vk_frame_end no-ops. */
	g_surface = vulkan_platform_create_surface(g_instance);

	if (!pick_physical_device())
		return;
	if (!create_device())
		return;

	if (g_surface) {
		if (!create_swapchain() || !create_frame_resources()) {
			g_log->write(LOG_LEVEL_ERROR,
				     "renderer_vulkan: swapchain bring-up "
				     "failed — no present this session");
		} else {
			g_have_present = 1;
		}
	} else if (vulkan_platform_hosted()) {
		/* A host means a window was expected, so this is a real error,
		 * not the supported headless answer. Say the most likely cause
		 * out loud: the host's extensions are collected during instance
		 * creation, so a host registered after that point contributes
		 * none and every VkCreate*SurfaceKHR is then illegal. */
		g_log->write(LOG_LEVEL_ERROR,
			     "renderer_vulkan: a window host is registered but "
			     "gave us no surface — nothing to present into. If "
			     "the instance line above reads 0 from window host, "
			     "the host was registered too late (it must be set "
			     "before the backend boots)");
	} else {
		g_log->write(LOG_LEVEL_WARN,
			     "renderer_vulkan: no window surface — instance and "
			     "device up, but nothing to present into");
	}

	g_ready = 1;
	g_log->write(LOG_LEVEL_INFO, "renderer_vulkan: device ready%s",
		     g_have_present ? " (presenting)" : " (headless)");
}

static void renderer_vulkan_shutdown(void)
{
	uint32_t i;

	if (g_device)
		vkDeviceWaitIdle(g_device);

	for (i = 0; i < VK_MAX_SWAP_IMAGES; i++)
		if (g_present_sems[i])
			vkDestroySemaphore(g_device, g_present_sems[i], NULL);
	for (i = 0; i < VK_FRAMES_IN_FLIGHT; i++) {
		if (g_acquire_sems[i])
			vkDestroySemaphore(g_device, g_acquire_sems[i], NULL);
		if (g_frame_fences[i])
			vkDestroyFence(g_device, g_frame_fences[i], NULL);
	}
	if (g_cmd_pool)
		vkDestroyCommandPool(g_device, g_cmd_pool, NULL);
	destroy_swapchain();

	/* The surface goes before the instance that owns it, and after the
	 * device/swapchain that used it. */
	if (g_surface)
		vkDestroySurfaceKHR(g_instance, g_surface, NULL);
	if (g_device)
		vkDestroyDevice(g_device, NULL);
	destroy_debug_messenger();
	if (g_instance)
		vkDestroyInstance(g_instance, NULL);

	memset(g_present_sems, 0, sizeof(g_present_sems));
	memset(g_acquire_sems, 0, sizeof(g_acquire_sems));
	memset(g_frame_fences, 0, sizeof(g_frame_fences));
	g_cmd_pool     = VK_NULL_HANDLE;
	g_surface      = VK_NULL_HANDLE;
	g_device       = VK_NULL_HANDLE;
	g_queue        = VK_NULL_HANDLE;
	g_instance     = VK_NULL_HANDLE;
	g_phys         = VK_NULL_HANDLE;
	g_ready        = 0;
	g_have_present = 0;
	g_log->write(LOG_LEVEL_INFO, "renderer_vulkan: shutdown");
}

static const struct subsystem desc = {
	.name     = "renderer",
	.api      = &vulkan_api,
	.init     = renderer_vulkan_init,
	.shutdown = renderer_vulkan_shutdown,
};

void renderer_vulkan_plugin_entry(struct subsystem_manager *mgr)
{
	const struct log_api *log = subsystem_manager_get_api(mgr, "log");

	if (log)
		g_log = log;
	subsystem_manager_register(mgr, &desc);
}

int renderer_vulkan_device_ready(void)
{
	return g_ready;
}
